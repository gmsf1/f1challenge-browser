#ifndef RC_RUNTIME_H
#define RC_RUNTIME_H

#include "cpu.h"
#include <ctype.h>
#include <setjmp.h>

extern jmp_buf *g_trap_jmp;
extern char g_trap_msg[256];

/* Guest address-space layout. The executable sits at its preferred base (usually 0x00400000), below
   the heap. */
#define GUEST_TIB              0x00010000u
#define GUEST_STACK_TOP        0x00300000u
#define GUEST_STACK_SIZE       0x00200000u
#define GUEST_HEAP_BASE        0x10000000u
/* Natively the guest gets a full 4 GiB reservation. In the browser (wasm32) it must share a 4 GiB
   memory with everything else, so the heap and VirtualAlloc ranges are compacted: Cricket peaked at
   ~125 MB of heap; F1 track loading needs more (no coalescing in heap_alloc), hence 1 GB of heap and 768 MB of virtual allocations (mostly mapped archives).
   The upscaled textures of tools/upscale_mas.py are four times the pixels (the 2002 cars, tracks with --grain) and the archives are read into the
   heap rather than mapped (virtual allocations stay near 0 MB in the sessions tried): 1.5 GB of heap and 256 MB of virtual allocations. Guest pointers
   stay below 0x80000000, as the game compares some of them as signed. */
#ifdef __EMSCRIPTEN__
#define GUEST_SPACE            0x80000000ull
#define GUEST_HEAP_HI          0x70000000u
#define GUEST_VALLOC_LO        0x70000000u
#define GUEST_VALLOC_HI        0x80000000u
#else
#define GUEST_SPACE            0x100000000ull
#define GUEST_HEAP_HI          0x60000000u
#define GUEST_VALLOC_LO        0x60000000u
#define GUEST_VALLOC_HI        0xE0000000u
#endif
#define GUEST_RETURN_SENTINEL  0xDEADBEEFu

typedef struct { const char *name; GuestFn fn; } ImportHandler;  /* name like "KERNEL32.DLL!GetTickCount" */
typedef struct { uint32_t image_base, entry, num_imports; } LoadedImage;
extern uint32_t g_image_lo, g_image_hi;   /* the loaded executable's address range */
extern uint32_t g_state_lo, g_state_hi;   /* its writable sections (global data): what determinism checks hash */

void runtime_init(CPU *c);
void set_import_handlers(ImportHandler *h, uint32_t n);  /* call before load_image */
int load_image(const char *path, LoadedImage *out);
GuestFn lookup_function(uint32_t addr);
const char *import_name(uint32_t addr);
uint32_t add_dynamic_import(const char *name, GuestFn fn);   /* for GetProcAddress */
GuestFn find_import_handler(const char *name);
extern __thread uint32_t g_import_target;  /* address of the host slot being called (set by call_indirect) */
uint32_t guest_alloc(uint32_t size);
void guest_call(CPU *c, GuestFn fn, int nargs, const uint32_t *args);
void register_all_functions(void);  /* generated */

/* Code created at run time (e.g. sound-engine plug-ins loaded from data files): interpreted, see interp.c */
void dyncode_register(uint32_t addr, uint32_t len);
int dyncode_contains(uint32_t addr);
void interp_call(CPU *c, uint32_t eip);   /* return address already pushed */

typedef struct { uint32_t addr; uint32_t regs[8]; double st0; uint32_t top; } TraceRec;
/* setjmp/longjmp over recompiled code: longjmp restores the guest registers and sets g_unwind; every call site then
   returns early until the frame that called setjmp (it recognises its own native frame) resumes after the call. */
extern __thread volatile int g_unwind;
void rc_setjmp_note(void *frame, uint32_t eip);   /* generated code, just before a call to _setjmp3 */
uint32_t rc_unwind_here(void *frame);             /* eip to resume at if this frame is the target, else 0 */
void host_setjmp3(CPU *c);
void host_longjmp(CPU *c);
extern volatile int g_fn_trace;            /* RC_TRACE_FN=1: log each guest function the first time it runs */
void fn_enter(uint32_t addr);
#define FN_ENTER(a) do { if (__builtin_expect(g_fn_trace, 0)) fn_enter(a); } while (0)
extern TraceRec *g_trace;          /* set by a test harness to record every instruction */
extern uint32_t g_trace_len, g_trace_cap;

#endif
