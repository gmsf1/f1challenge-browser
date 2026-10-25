/* Windows cursors for the game. It loads Options/CURSOR0n.cur with LoadImageA(IMAGE_CURSOR, LR_LOADFROMFILE),
 * switches with SetCursor and hides/shows the system cursor with ShowCursor, expecting Windows to draw it. Here
 * the .cur is decoded to RGBA and handed to the page, which shows it as the CSS cursor of the game canvas. */
#include "win32.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

typedef struct { int w, h, hx, hy; uint8_t *rgba; } Cursor;
#define MAX_CURSORS 16
#define HCURSOR_BASE 0x00060000u
static Cursor g_cursors[MAX_CURSORS];
static int g_ncursors;
static int g_current = -2;   /* index into g_cursors; -1: no cursor (SetCursor(NULL)); -2: the default arrow */
static int g_visible = 1;    /* the ShowCursor display counter is >= 0 */

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* .cur / .ico: ICONDIR, then a DIB per image (XOR colours, then a 1-bit AND mask; both stored bottom-up). */
static int parse_cur(const uint8_t *f, size_t n, Cursor *out) {
    if (n < 22 || le16(f) != 0) return 0;
    uint32_t type = le16(f + 2);
    if ((type != 1 && type != 2) || le16(f + 4) < 1) return 0;
    const uint8_t *e = f + 6;
    int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
    uint32_t off = le32(e + 12);
    if (off + 40 > n || le32(f + off) < 40) return 0;
    const uint8_t *dib = f + off;
    uint32_t bpp = le16(dib + 14), comp = le32(dib + 16), used = le32(dib + 32), hdr = le32(dib);
    if (comp != 0 || (int)le32(dib + 4) != w) return 0;
    uint32_t pal = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    size_t xor_off = off + hdr + pal * 4, xor_row = (((size_t)w * bpp + 31) / 32) * 4;
    size_t and_off = xor_off + xor_row * h, and_row = (((size_t)w + 31) / 32) * 4;
    if (and_off + and_row * h > n) return 0;
    uint8_t *px = calloc((size_t)w * h, 4);
    int has_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *xr = f + xor_off + xor_row * (h - 1 - y), *ar = f + and_off + and_row * (h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t b = 0, g = 0, r = 0, a = 255;
            if (bpp == 32) { b = xr[x * 4]; g = xr[x * 4 + 1]; r = xr[x * 4 + 2]; a = xr[x * 4 + 3]; has_alpha |= a != 0; }
            else if (bpp == 24) { b = xr[x * 3]; g = xr[x * 3 + 1]; r = xr[x * 3 + 2]; }
            else if (bpp == 8 || bpp == 4 || bpp == 1) {
                uint32_t idx = bpp == 8 ? xr[x] : bpp == 4 ? (xr[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (xr[x / 8] >> (7 - (x & 7))) & 1;
                const uint8_t *c = f + off + hdr + idx * 4;
                b = c[0]; g = c[1]; r = c[2];
            } else { free(px); return 0; }
            int masked = (ar[x / 8] >> (7 - (x & 7))) & 1;   /* 1: transparent (or, with XOR 1, inverted: shown white) */
            if (bpp != 32 || !has_alpha) a = masked ? 0 : 255;
            if (masked && bpp == 1 && (xr[x / 8] >> (7 - (x & 7))) & 1) { r = g = b = 255; a = 255; }
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
    if (bpp == 32 && !has_alpha) {   /* 32-bit without any alpha: the AND mask decides */
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const uint8_t *ar = f + and_off + and_row * (h - 1 - y);
            px[((size_t)y * w + x) * 4 + 3] = (ar[x / 8] >> (7 - (x & 7))) & 1 ? 0 : 255;
        }
    }
    *out = (Cursor){ w, h, type == 2 ? (int)le16(e + 4) : w / 2, type == 2 ? (int)le16(e + 6) : h / 2, px };
    return 1;
}

static void apply(void) {
#ifdef __EMSCRIPTEN__
    int show = g_visible && g_current != -1;
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof document === 'undefined') return;   // headless (Node)
        const c = document.getElementById('canvas');
        if (!c) return;
        c.style.cursor = $0 ? ((Module.rcCursors && Module.rcCursors[$1]) || 'default') : 'none';
    }, show, g_current);
#endif
}

uint32_t cursor_load_image(uint32_t name, uint32_t type, uint32_t flags) {
    if (type != 2 /* IMAGE_CURSOR */ || !(flags & 0x10 /* LR_LOADFROMFILE */) || g_ncursors >= MAX_CURSORS) return 0x00030003u;
    char host[1024];
    if (!map_path(GSTR(name), host, sizeof host)) return 0x00030003u;
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0x00030003u;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    Cursor cur;
    int ok = got == (size_t)n && parse_cur(buf, (size_t)n, &cur);
    free(buf);
    LOG("[cursor] LoadImageA(%s) -> %s\n", GSTR(name), ok ? "decoded" : "not a cursor");
    if (!ok) return 0x00030003u;
    int idx = g_ncursors++;
    g_cursors[idx] = cur;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof document === 'undefined') return;   // headless (Node)
        const buf = new Uint8Array(wasmMemory.buffer, $0 >>> 0, $1).slice();
        const c = document.createElement('canvas');
        c.width = $2; c.height = $3;
        c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(buf.buffer), $2, $3), 0, 0);
        (Module.rcCursors = Module.rcCursors || {})[$6] = 'url(' + c.toDataURL() + ') ' + $4 + ' ' + $5 + ', auto';
    }, cur.rgba, cur.w * cur.h * 4, cur.w, cur.h, cur.hx, cur.hy, idx);
#endif
    return HCURSOR_BASE + (uint32_t)idx;
}

void cursor_set(uint32_t h) {
    g_current = h == 0 ? -1 : (h >= HCURSOR_BASE && h < HCURSOR_BASE + MAX_CURSORS) ? (int)(h - HCURSOR_BASE) : -2;
    apply();
}

void cursor_show(int32_t display_count) {
    g_visible = display_count >= 0;
    apply();
}
