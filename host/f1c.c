/* Imports F1 Challenge 99-02 needs on top of the base layer: a few KERNEL32/USER32/SHELL32/ADVAPI32 calls, Winsock
 * (always "no network"), Miles Sound System, Bink video and AVI. Miles and Bink fail safe for now: the game starts
 * with no sound driver and no movies. Real audio would route into dsound.c's mixer; movies need a Bink decoder. */
#include "win32.h"

#include <sys/stat.h>

#define W(name) static void w_##name(CPU *c)

/* ================================================================== KERNEL32 */
W(GetVersion) { RET(0, 0x0A280105u); }   /* Windows XP: 5.1, build 2600 */
W(LocalAlloc) { RET(2, heap_alloc(ARG(1), ARG(0) & 0x40)); }
W(GetEnvironmentVariableA) { RET(3, 0); }
W(GetFileAttributesA) {
    char host[1024];
    struct stat st;
    if (!map_path(GSTR(ARG(0)), host, sizeof host) || stat(host, &st)) {
        LOG("[file] GetFileAttributesA(%s) = none\n", GSTR(ARG(0)));
        if (t_self) t_self->last_error = 2;
        RET(1, 0xFFFFFFFFu);
    }
    LOG("[file] GetFileAttributesA(%s) = %s\n", GSTR(ARG(0)), S_ISDIR(st.st_mode) ? "dir" : "file");
    RET(1, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}
W(SetFileAttributesA) { RET(2, 1); }
W(GetFileInformationByHandle) { RET(2, 0); }
W(PeekNamedPipe) { RET(6, 0); }
W(CreateProcessA) { RET(10, 0); }
W(CopyFileA) { if (t_self) t_self->last_error = 5; RET(3, 0); }
W(QueueUserAPC) { RET(3, 0); }
W(SuspendThread) { RET(1, 0); }
W(SetPriorityClass) { RET(2, 1); }
W(GetPriorityClass) { RET(1, 0x20); }
W(CompareFileTime) {
    uint64_t a = R64(ARG(0)), b = R64(ARG(1));
    RET(2, a < b ? -1 : a > b);
}
static void put_systemtime(uint32_t p, const struct tm *t, unsigned ms) {
    W16(p, (uint16_t)(t->tm_year + 1900)); W16(p + 2, (uint16_t)(t->tm_mon + 1)); W16(p + 4, (uint16_t)t->tm_wday);
    W16(p + 6, (uint16_t)t->tm_mday); W16(p + 8, (uint16_t)t->tm_hour); W16(p + 10, (uint16_t)t->tm_min);
    W16(p + 12, (uint16_t)t->tm_sec); W16(p + 14, (uint16_t)ms);
}
W(GetSystemTime) {
    uint64_t ft = guest_time_100ns();
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    put_systemtime(ARG(0), &tmv, (unsigned)((ft / 10000) % 1000));
    RET(1, 0);
}
W(GetLocalTime) {
    uint64_t ft = guest_time_100ns();
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tmv;
    localtime_r(&t, &tmv);
    put_systemtime(ARG(0), &tmv, (unsigned)((ft / 10000) % 1000));
    RET(1, 0);
}
W(GlobalMemoryStatus) {   /* MEMORYSTATUS: a 512 MB machine */
    uint32_t p = ARG(0);
    W32(p, 32); W32(p + 4, 10); W32(p + 8, 0x20000000); W32(p + 12, 0x18000000);
    W32(p + 16, 0x40000000); W32(p + 20, 0x30000000); W32(p + 24, 0x7FFE0000); W32(p + 28, 0x70000000);
    RET(1, 0);
}
#define K(name) IMPORT("KERNEL32.DLL", name)
static const ImportHandler k_list[] = {
    K(GetVersion), K(LocalAlloc), K(GetEnvironmentVariableA), K(GetFileAttributesA), K(SetFileAttributesA),
    K(GetFileInformationByHandle), K(PeekNamedPipe), K(CreateProcessA), K(CopyFileA), K(QueueUserAPC),
    K(SuspendThread), K(SetPriorityClass), K(GetPriorityClass), K(CompareFileTime), K(GetSystemTime),
    K(GetLocalTime), K(GlobalMemoryStatus),
};

/* ================================================================== USER32 / SHELL32 */
W(ClipCursor) { RET(1, 1); }
W(DestroyCursor) { RET(1, 1); }
W(SHGetSpecialFolderPathA) {   /* (hwnd, path, csidl, create): the game saves under here */
    LOG("[shell32] SHGetSpecialFolderPathA(csidl %u)\n", ARG(2));
    strcpy((char *)HP(ARG(1)), "C:\\Documents and Settings\\Player\\My Documents");
    RET(4, 1);
}

/* ================================================================== ADVAPI32 (security descriptors: all allowed) */
#define OK(name, n) W(name) { RET(n, 1); }
OK(SetSecurityDescriptorOwner, 3) OK(SetSecurityDescriptorGroup, 3) OK(SetSecurityDescriptorDacl, 4)
OK(InitializeSecurityDescriptor, 2) OK(IsValidSecurityDescriptor, 1) OK(InitializeAcl, 3)
OK(AddAccessAllowedAce, 4) OK(RevertToSelf, 0) OK(ImpersonateSelf, 1)
W(FreeSid) { RET(1, 0); }
W(GetLengthSid) { RET(1, 12); }
W(OpenThreadToken) { W32(ARG(3), 0x00090001u); RET(4, 1); }
W(OpenProcessToken) { W32(ARG(2), 0x00090002u); RET(3, 1); }
W(AllocateAndInitializeSid) { W32(ARG(10), heap_alloc(16, 1)); RET(11, 1); }
W(AccessCheck) { W32(ARG(6), ARG(2)); W32(ARG(7), 1); RET(8, 1); }   /* granted = desired */
#define A(name) IMPORT("ADVAPI32.DLL", name)
static const ImportHandler a_list[] = {
    A(SetSecurityDescriptorOwner), A(SetSecurityDescriptorGroup), A(SetSecurityDescriptorDacl),
    A(InitializeSecurityDescriptor), A(IsValidSecurityDescriptor), A(InitializeAcl), A(AddAccessAllowedAce),
    A(RevertToSelf), A(ImpersonateSelf), A(FreeSid), A(GetLengthSid), A(OpenThreadToken), A(OpenProcessToken),
    A(AllocateAndInitializeSid), A(AccessCheck),
};

/* WSOCK32 is in winsock.c (a virtual LAN over a relay; without ?lan the game runs offline) */
/* ================================================================== Miles Sound System (mss32): no driver */
/* X(name, argument bytes, return value) */
#define MSS_LIST(X) \
    X(AIL_set_preference, 8, 0) X(AIL_waveOutClose, 4, 0) X(AIL_set_DirectSound_HWND, 8, 0) X(AIL_enumerate_filters, 12, 0) \
    X(AIL_open_filter, 8, 0) X(AIL_set_filter_sample_preference, 12, 0) X(AIL_set_sample_processor, 12, 0)
/* (the calls that do something are in miles.c) */
/* ================================================================== Bink (binkw32): nothing opens, so no movies play */
#define BINK_LIST(X) \
    X(BinkSetVolume, 12, 0) X(BinkDoFrame, 4, 0) X(BinkCopyToBuffer, 28, 0) \
    X(BinkOpenMiles, 4, 0) X(BinkSetSoundSystem, 8, 1) X(BinkOpenDirectSound, 4, 0) X(BinkSetSoundTrack, 8, 0)
#define DEFSTUB(name, bytes, ret) W(name) { static int once; if (!once++) STUB(#name "\n"); RET((bytes) / 4, ret); }   /* logged once: BinkDoFrame runs every frame */
MSS_LIST(DEFSTUB)
BINK_LIST(DEFSTUB)
/* A stand-in movie: one black frame, so the game's movie player runs to its end and moves on. BINK header:
   +0 Width, +4 Height, +8 Frames, +12 FrameNum, +16 LastFrameNum, +20 FrameRate, +24 FrameRateDiv. */
W(BinkOpen) {
    char host[1024];
    if (!map_path(GSTR(ARG(0)), host, sizeof host)) {   /* no such movie (stripped install): the game skips it */
        LOG("[bink] BinkOpen(%s) -> missing\n", GSTR(ARG(0)));
        RET(2, 0);
    }
    LOG("[bink] BinkOpen(%s) -> fake movie\n", GSTR(ARG(0)));
    uint32_t b = heap_alloc(0x200, 1);
    /* menu backdrops (...BK.BIK) loop for as long as the screen is up; intros end at once */
    int bg = strstr(GSTR(ARG(0)), "BK.BIK") || strstr(GSTR(ARG(0)), "bk.bik");
    /* the picture size comes from the file (BIK header: width +20, height +24); texture movies must be a power of two */
    uint32_t w = 640, h = 480, hdr[8];
    FILE *fp = fopen(host, "rb");
    if (fp) {
        if (fread(hdr, 4, 8, fp) == 8 && !memcmp(hdr, "BIK", 3) && hdr[5] && hdr[6] && hdr[5] <= 4096 && hdr[6] <= 4096) { w = hdr[5]; h = hdr[6]; }
        fclose(fp);
    }
    W32(b, w); W32(b + 4, h); W32(b + 8, bg ? 0x7fffffff : 1); W32(b + 12, 0); W32(b + 16, 0); W32(b + 20, 30); W32(b + 24, 1);
    RET(2, b);
}
/* BINKSUMMARY: the player reads Width and Height (the first two fields) to size its picture */
W(BinkGetSummary) {
    uint32_t p = ARG(1);
    memset(HP(p), 0, 128);
    W32(p, R32(ARG(0))); W32(p + 4, R32(ARG(0) + 4)); W32(p + 32, R32(ARG(0) + 8));
    RET(2, 0);
}
W(BinkClose) { heap_free(ARG(0)); RET(1, 0); }
W(BinkNextFrame) { uint32_t b = ARG(0); W32(b + 16, R32(b + 12)); W32(b + 12, R32(b + 12) + 1); RET(1, 0); }
W(BinkWait) { RET(1, 0); }   /* 0: the next frame is due now */
W(BinkGetError) { RET(0, guest_strdup("movies are not supported in this build")); }

/* ================================================================== AVIFIL32 */
W(AVIFileExit) { RET(0, 0); }
W(AVIFileRelease) { RET(1, 0); }
W(AVIStreamRelease) { RET(1, 0); }

#define STR2(x) #x
#define MSS_ENT(name, bytes, ret) { "MSS32.DLL!_" #name "@" STR2(bytes), w_##name },
#define BINK_ENT(name, bytes, ret) { "BINKW32.DLL!_" #name "@" STR2(bytes), w_##name },
static const ImportHandler m_list[] = {
    MSS_LIST(MSS_ENT) BINK_LIST(BINK_ENT)
    { "BINKW32.DLL!_BinkOpen@8", w_BinkOpen }, { "BINKW32.DLL!_BinkGetSummary@8", w_BinkGetSummary }, { "BINKW32.DLL!_BinkClose@4", w_BinkClose },
    { "BINKW32.DLL!_BinkNextFrame@4", w_BinkNextFrame }, { "BINKW32.DLL!_BinkWait@4", w_BinkWait },
    { "BINKW32.DLL!_BinkGetError@0", w_BinkGetError },
    IMPORT("AVIFIL32.DLL", AVIFileExit), IMPORT("AVIFIL32.DLL", AVIFileRelease), IMPORT("AVIFIL32.DLL", AVIStreamRelease),
    IMPORT("USER32.DLL", ClipCursor), IMPORT("USER32.DLL", DestroyCursor),
    IMPORT("SHELL32.DLL", SHGetSpecialFolderPathA),
};

/* One table for main.c: the lists above are concatenated at start-up. */
#define NLIST(l) (sizeof l / sizeof l[0])
const ImportHandler *miles_imports(uint32_t *n);
static ImportHandler g_all[NLIST(k_list) + NLIST(a_list) + NLIST(m_list) + 120];
const ImportHandler *winsock_imports(uint32_t *n);
static ImportTable build(void) {
    uint32_t n = 0;
    for (size_t i = 0; i < NLIST(k_list); i++) g_all[n++] = k_list[i];
    for (size_t i = 0; i < NLIST(a_list); i++) g_all[n++] = a_list[i];
    uint32_t nw;
    const ImportHandler *wl = winsock_imports(&nw);
    for (uint32_t i = 0; i < nw; i++) g_all[n++] = wl[i];
    for (size_t i = 0; i < NLIST(m_list); i++) g_all[n++] = m_list[i];
    uint32_t nm;
    const ImportHandler *ml = miles_imports(&nm);
    for (uint32_t i = 0; i < nm; i++) g_all[n++] = ml[i];
    return (ImportTable){ g_all, n };
}
const ImportTable *f1c_imports(void) { static ImportTable t; if (!t.list) t = build(); return &t; }
