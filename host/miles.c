/* Miles Sound System (mss32) for F1 Challenge: WAV samples and MP3/WAV streams, mixed in software.
 * The game uses only a few dozen Miles calls: one digital driver, sample handles (a WAV image from AIL_file_read
 * or its own archive), streams for the music (MP3, decoded by Mp3dec.asi on Windows, by minimp3 here) and an
 * end-of-stream callback. There is no 3D audio, no filters and no reverb.
 * Voices are mixed by dsound.c's mixer thread into the output ring (miles_mix); callbacks are made from AIL_serve
 * and the other Miles calls, on the game's thread. */
#include "win32.h"

#include <math.h>
#include <pthread.h>

#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3.h"

#define W(name) static void w_##name(CPU *c)
#define MIX_RATE 44100
#define MAX_VOICES 96
#define SMP_FREE 1
#define SMP_DONE 2
#define SMP_PLAYING 4
#define SMP_STOPPED 8

void dsound_start_mixer(void);   /* dsound.c */

typedef struct {
    uint32_t handle;           /* guest-visible handle (0: slot unused) */
    int stream;
    int16_t *pcm;              /* interleaved, `channels` per frame (host memory) */
    uint32_t frames, cap;      /* frames available / allocated (a stream's decoder thread appends) */
    int channels;
    uint32_t rate;             /* playback rate in Hz */
    double pos;                /* frames */
    int loops;                 /* 0: forever, n: play n times */
    int done_loops;
    int state;                 /* SMP_* */
    float vl, vr;
    int decoding;              /* a stream whose decoder thread is still running */
    uint32_t callback;         /* stream end callback (guest function) */
    int callback_due;
} Voice;

static Voice g_voices[MAX_VOICES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_open_cb, g_close_cb, g_seek_cb, g_read_cb;   /* the game's file callbacks */

static float fl(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static Voice *voice_of(uint32_t h) {
    for (int i = 0; i < MAX_VOICES; i++) if (g_voices[i].handle == h && h) return &g_voices[i];
    return NULL;
}
static Voice *voice_new(int stream) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i].handle) {
            Voice *v = &g_voices[i];
            memset(v, 0, sizeof *v);
            v->handle = heap_alloc(16, 1);   /* an opaque, unique pointer */
            v->stream = stream; v->loops = 1; v->vl = v->vr = 1.f; v->state = SMP_DONE; v->rate = MIX_RATE;
            return v;
        }
    return NULL;
}
static void voice_free(Voice *v) {
    pthread_mutex_lock(&g_lock);
    free(v->pcm);
    heap_free(v->handle);
    memset(v, 0, sizeof *v);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ mixing */

void miles_mix(float *out, int frames) {
    pthread_mutex_lock(&g_lock);
    for (int n = 0; n < MAX_VOICES; n++) {
        Voice *v = &g_voices[n];
        if (!v->handle || v->state != SMP_PLAYING) continue;
        double step = (double)v->rate / MIX_RATE;
        for (int i = 0; i < frames && v->state == SMP_PLAYING; i++) {
            uint32_t f = (uint32_t)v->pos;
            if (f + 1 >= v->frames) {
                if (v->decoding) break;   /* the decoder is behind: wait (silence) */
                if (f >= v->frames || v->frames == 0) goto ended;
            }
            {
                double fr = v->pos - f;
                uint32_t g = f + 1 < v->frames ? f + 1 : f;
                const int16_t *a = v->pcm + (size_t)f * v->channels, *b = v->pcm + (size_t)g * v->channels;
                float l = (float)(a[0] + (b[0] - a[0]) * fr) / 32768.f;
                float r = v->channels == 2 ? (float)(a[1] + (b[1] - a[1]) * fr) / 32768.f : l;
                out[2 * i] += l * v->vl;
                out[2 * i + 1] += r * v->vr;
            }
            v->pos += step;
            if ((uint32_t)v->pos >= v->frames && !v->decoding) {
            ended:
                if (v->loops == 0 || ++v->done_loops < v->loops) v->pos = 0;
                else { v->state = SMP_DONE; v->pos = 0; if (v->stream && v->callback) v->callback_due = 1; }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (g_log) {   /* RC_LOG: peak level of what Miles mixed, about once a second */
        static float peak; static int n;
        for (int i = 0; i < 2 * frames; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
        if ((n += frames) >= MIX_RATE) { if (peak > 0) LOG("[miles] mix peak %.3f\n", peak); n = 0; peak = 0; }
    }
}

/* ------------------------------------------------------------------ loading */

/* Reads a whole file through the game's callbacks (its archive layer) or, failing that, from the host. */
static uint8_t *read_file(CPU *c, const char *name, uint32_t *size) {
    if (g_open_cb && g_read_cb) {
        uint32_t hp = heap_alloc(8, 1), a1[2] = { guest_strdup(name), hp };
        uint32_t ok = guest_stdcall(c, g_open_cb, 2, a1);
        uint32_t fh = R32(hp);
        heap_free(hp);
        if (ok) {
            /* the size comes from seeking to the end: a read callback isn't guaranteed to stop at the end of the file */
            uint32_t total = 0;
            if (g_seek_cb) {
                uint32_t a[3] = { fh, 0, 2 /* AIL_FILE_SEEK_END */ };
                total = guest_stdcall(c, g_seek_cb, 3, a);
                uint32_t b[3] = { fh, 0, 0 /* AIL_FILE_SEEK_BEGIN */ };
                guest_stdcall(c, g_seek_cb, 3, b);
            }
            LOG("[miles] callback file %s: size %u\n", name, total);
            uint32_t cap = total && total < (256u << 20) ? total : 1 << 20, n = 0;
            uint8_t *buf = malloc(cap);
            uint32_t chunk = heap_alloc(65536, 0);
            for (;;) {
                uint32_t want = total ? (total - n < 65536 ? total - n : 65536) : 65536;
                if (!want) break;
                uint32_t a[3] = { fh, chunk, want };
                uint32_t got = guest_stdcall(c, g_read_cb, 3, a);
                if (!got) break;
                if (n + got > cap) buf = realloc(buf, cap = (n + got) * 2);
                memcpy(buf + n, HP(chunk), got);
                n += got;
                if (got < want) break;
            }
            heap_free(chunk);
            if (g_close_cb) { uint32_t a[1] = { fh }; guest_stdcall(c, g_close_cb, 1, a); }
            *size = n;
            return buf;
        }
    }
    char host[1024];
    if (!map_path(name, host, sizeof host)) return NULL;
    FILE *fp = fopen(host, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return NULL; }
    fclose(fp);
    *size = (uint32_t)n;
    return buf;
}

typedef struct { uint32_t rate, bits, channels, len; const uint8_t *data; } Wav;
static int parse_wav(const uint8_t *p, uint32_t n, Wav *w) {
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    memset(w, 0, sizeof *w);
    for (uint32_t o = 12; o + 8 <= n;) {
        uint32_t len = le32(p + o + 4);
        if (!memcmp(p + o, "fmt ", 4) && o + 24 <= n) {
            if (le16(p + o + 8) != 1) return 0;   /* PCM only */
            w->channels = le16(p + o + 10); w->rate = le32(p + o + 12); w->bits = le16(p + o + 22);
        } else if (!memcmp(p + o, "data", 4)) {
            w->data = p + o + 8; w->len = o + 8 + len > n ? n - o - 8 : len;
            return w->channels && w->rate && (w->bits == 8 || w->bits == 16);
        }
        o += 8 + len + (len & 1);
    }
    return 0;
}

/* WAV -> interleaved int16 */
static int load_wav(Voice *v, const uint8_t *p, uint32_t n) {
    Wav w;
    if (!parse_wav(p, n, &w) || w.channels > 2) return 0;
    uint32_t bytes = w.bits / 8, frames = w.len / (bytes * w.channels);
    int16_t *pcm = malloc((size_t)frames * w.channels * 2 + 4);
    for (uint32_t i = 0; i < frames * w.channels; i++)
        pcm[i] = w.bits == 16 ? (int16_t)le16(w.data + 2 * i) : (int16_t)((w.data[i] - 128) << 8);
    pthread_mutex_lock(&g_lock);
    free(v->pcm);
    v->pcm = pcm; v->frames = frames; v->cap = frames; v->channels = (int)w.channels; v->rate = w.rate; v->pos = 0;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

/* MP3: a thread decodes while the voice already plays what exists */
typedef struct { Voice *v; uint8_t *data; uint32_t size; } Mp3Job;
static void *mp3_thread(void *arg) {
    Mp3Job *j = arg;
    mp3dec_t dec;
    mp3dec_init(&dec);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t off = 0;
    while (off < j->size) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, j->data + off, (int)(j->size - off), pcm, &info);
        if (!info.frame_bytes) break;
        off += (uint32_t)info.frame_bytes;
        if (!samples || info.channels < 1 || info.channels > 2) continue;
        pthread_mutex_lock(&g_lock);
        Voice *v = j->v;
        if (!v->handle) { pthread_mutex_unlock(&g_lock); break; }   /* closed meanwhile */
        if (!v->channels) { v->channels = info.channels; v->rate = (uint32_t)info.hz; }
        if (v->frames + (uint32_t)samples > v->cap) {
            v->cap = v->cap ? v->cap * 2 : 1 << 18;
            v->pcm = realloc(v->pcm, (size_t)v->cap * v->channels * 2);
        }
        memcpy(v->pcm + (size_t)v->frames * v->channels, pcm, (size_t)samples * v->channels * 2);
        v->frames += (uint32_t)samples;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_lock(&g_lock);
    if (j->v->handle) { j->v->decoding = 0; LOG("[miles] mp3 decoded: %u frames at %u Hz (%.1f s)\n", j->v->frames, j->v->rate, j->v->rate ? (double)j->v->frames / j->v->rate : 0.0); }
    pthread_mutex_unlock(&g_lock);
    free(j->data);
    free(j);
    return NULL;
}

static void service(CPU *c) {   /* stream end callbacks, on the game's thread */
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (!v->handle || !v->callback_due) continue;
        v->callback_due = 0;
        uint32_t a[1] = { v->handle };
        guest_stdcall(c, v->callback, 1, a);
    }
}

/* ------------------------------------------------------------------ the API */

W(AIL_startup) { RET(0, 1); }
W(AIL_shutdown) { RET(0, 0); }
W(AIL_open_digital_driver) { dsound_start_mixer(); LOG("[miles] digital driver open\n"); RET(4, 0x00B10000); }
W(AIL_serve) { service(c); RET(0, 0); }
W(AIL_set_file_callbacks) { g_open_cb = ARG(0); g_close_cb = ARG(1); g_seek_cb = ARG(2); g_read_cb = ARG(3); RET(4, 0); }
W(AIL_mem_free_lock) { heap_free(ARG(0)); RET(1, 0); }
W(AIL_file_read) {
    uint32_t n = 0;
    uint8_t *b = read_file(c, GSTR(ARG(0)), &n);
    LOG("[miles] file_read(%s) -> %s (%u bytes)\n", GSTR(ARG(0)), b ? "ok" : "missing", n);
    if (!b) RET(2, 0);
    uint32_t dst = ARG(1) ? ARG(1) : heap_alloc(n + 1, 0);
    memcpy(HP(dst), b, n);
    free(b);
    RET(2, dst);
}
W(AIL_WAV_info) {   /* AILSOUNDINFO: format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr */
    Wav w;
    uint32_t info = ARG(1);
    if (!parse_wav(HP(ARG(0)), 0x7fffffff, &w)) RET(2, 0);
    W32(info, 1); W32(info + 4, ARG(0) + (uint32_t)(w.data - (const uint8_t *)HP(ARG(0)))); W32(info + 8, w.len);
    W32(info + 12, w.rate); W32(info + 16, w.bits); W32(info + 20, w.channels);
    W32(info + 24, w.len / (w.bits / 8 * w.channels)); W32(info + 28, w.bits / 8 * w.channels); W32(info + 32, 0);
    RET(2, 1);
}

W(AIL_allocate_sample_handle) { Voice *v = voice_new(0); RET(1, v ? v->handle : 0); }
W(AIL_release_sample_handle) { Voice *v = voice_of(ARG(0)); if (v) voice_free(v); RET(1, 0); }
W(AIL_init_sample) {
    Voice *v = voice_of(ARG(0));
    if (v) {
        pthread_mutex_lock(&g_lock);
        free(v->pcm);
        v->pcm = NULL; v->frames = v->cap = 0; v->state = SMP_DONE; v->loops = 1; v->done_loops = 0; v->pos = 0; v->vl = v->vr = 1.f;
        pthread_mutex_unlock(&g_lock);
    }
    RET(1, 0);
}
W(AIL_set_sample_file) {
    Voice *v = voice_of(ARG(0));
    int ok = v && load_wav(v, HP(ARG(1)), 0x7fffffff);
    if (!ok) LOG("[miles] set_sample_file: not a PCM WAV\n");
    RET(3, ok);
}
W(AIL_set_sample_loop_count) { Voice *v = voice_of(ARG(0)); if (v) v->loops = (int)ARG(1); RET(2, 0); }
W(AIL_set_sample_playback_rate) { Voice *v = voice_of(ARG(0)); if (v && ARG(1)) v->rate = ARG(1); RET(2, 0); }
W(AIL_set_sample_position) {
    Voice *v = voice_of(ARG(0));
    if (v && v->channels) v->pos = (double)(ARG(1) / (2u * (uint32_t)v->channels));   /* bytes of 16-bit data */
    RET(2, 0);
}
W(AIL_set_sample_volume_levels) { Voice *v = voice_of(ARG(0)); if (v) { v->vl = fl(ARG(1)); v->vr = fl(ARG(2)); } RET(3, 0); }
W(AIL_start_sample) { Voice *v = voice_of(ARG(0)); if (v && v->frames) { v->pos = 0; v->done_loops = 0; v->state = SMP_PLAYING; } RET(1, 0); }
W(AIL_stop_sample) { Voice *v = voice_of(ARG(0)); if (v && v->state == SMP_PLAYING) v->state = SMP_STOPPED; RET(1, 0); }
W(AIL_resume_sample) { Voice *v = voice_of(ARG(0)); if (v && v->state == SMP_STOPPED) v->state = SMP_PLAYING; RET(1, 0); }
W(AIL_end_sample) { Voice *v = voice_of(ARG(0)); if (v) { v->state = SMP_DONE; v->pos = 0; } RET(1, 0); }
W(AIL_sample_status) { Voice *v = voice_of(ARG(0)); RET(1, v ? v->state : SMP_FREE); }

W(AIL_open_stream) {
    uint32_t n = 0;
    const char *name = GSTR(ARG(1));
    uint8_t *b = read_file(c, name, &n);
    LOG("[miles] open_stream(%s) -> %s (%u bytes)\n", name, b ? "ok" : "missing", n);
    if (!b) RET(3, 0);
    Voice *v = voice_new(1);
    if (!v) { free(b); RET(3, 0); }
    if (n > 12 && !memcmp(b, "RIFF", 4)) {
        int ok = load_wav(v, b, n);
        free(b);
        if (!ok) { voice_free(v); RET(3, 0); }
    } else {   /* MP3 */
        Mp3Job *j = calloc(1, sizeof *j);
        j->v = v; j->data = b; j->size = n;
        v->decoding = 1; v->state = SMP_STOPPED;
        pthread_t t;
        pthread_create(&t, NULL, mp3_thread, j);
        pthread_detach(t);
    }
    v->state = SMP_STOPPED;   /* opened paused: AIL_start_stream plays */
    RET(3, v->handle);
}
W(AIL_close_stream) { Voice *v = voice_of(ARG(0)); if (v) voice_free(v); RET(1, 0); }
W(AIL_start_stream) { Voice *v = voice_of(ARG(0)); if (v) { v->pos = 0; v->done_loops = 0; v->state = SMP_PLAYING; } RET(1, 0); }
W(AIL_pause_stream) {
    Voice *v = voice_of(ARG(0));
    if (v) { if (ARG(1)) { if (v->state == SMP_PLAYING) v->state = SMP_STOPPED; } else if (v->state == SMP_STOPPED) v->state = SMP_PLAYING; }
    RET(2, 0);
}
W(AIL_register_stream_callback) { Voice *v = voice_of(ARG(0)); if (v) v->callback = ARG(1); RET(2, 0); }
W(AIL_set_stream_volume_levels) { Voice *v = voice_of(ARG(0)); if (v) { v->vl = fl(ARG(1)); v->vr = fl(ARG(2)); } RET(3, 0); }

#define STR2(x) #x
#define ENT(name, bytes) { "MSS32.DLL!_" #name "@" STR2(bytes), w_##name }
static const ImportHandler g_list[] = {
    ENT(AIL_startup, 0), ENT(AIL_shutdown, 0), ENT(AIL_open_digital_driver, 16), ENT(AIL_serve, 0),
    ENT(AIL_set_file_callbacks, 16), ENT(AIL_mem_free_lock, 4), ENT(AIL_file_read, 8), ENT(AIL_WAV_info, 8),
    ENT(AIL_allocate_sample_handle, 4), ENT(AIL_release_sample_handle, 4), ENT(AIL_init_sample, 4),
    ENT(AIL_set_sample_file, 12), ENT(AIL_set_sample_loop_count, 8), ENT(AIL_set_sample_playback_rate, 8),
    ENT(AIL_set_sample_position, 8), ENT(AIL_set_sample_volume_levels, 12), ENT(AIL_start_sample, 4),
    ENT(AIL_stop_sample, 4), ENT(AIL_resume_sample, 4), ENT(AIL_end_sample, 4), ENT(AIL_sample_status, 4),
    ENT(AIL_open_stream, 12), ENT(AIL_close_stream, 4), ENT(AIL_start_stream, 4), ENT(AIL_pause_stream, 8),
    ENT(AIL_register_stream_callback, 8), ENT(AIL_set_stream_volume_levels, 12),
};
const ImportHandler *miles_imports(uint32_t *n) { *n = (uint32_t)(sizeof g_list / sizeof g_list[0]); return g_list; }
