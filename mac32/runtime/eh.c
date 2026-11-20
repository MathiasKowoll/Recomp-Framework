// C++ exceptions of the guest (the Itanium ABI as GCC 4.2 and llvm-gcc emit it for i386).
//
// A translated function is a C function, so the host's unwinder cannot be used to leave guest frames: an exception
// is a flag on the CPU instead. __cxa_throw sets it and returns; the translator has put a test after every call,
// and a function whose exception table has no landing pad for that call returns at once, so the flag travels up the
// C stack one frame at a time. Where the table does have a landing pad, the frame's registers are put back as they
// were at the call (what the real unwinder recovers from the frames below) and eh_land() decides, from the table's
// action list and the type thrown, whether that pad is for this exception: it then clears the flag, leaves the
// exception object in eax and the handler's index in edx, and the function goes to the pad, which was translated
// with the rest of it. A clean-up pad ends in _Unwind_Resume, which sets the flag again.
//
// There is one pass, not the ABI's two: clean-ups run on the way up even if nothing catches at the top (the real
// one would call terminate without running them). A catch of a base class does not match a derived class thrown
// unless eh_base_of() knows the hierarchy; a catch of the exact type, of `...`, and of a pointer to const do.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t eh_guest_alloc(uint32_t n);   // the guest's allocator, given by the runtime this is linked into
void eh_guest_free(uint32_t p);
__attribute__((weak)) int eh_base_of(uint32_t catch_ti, uint32_t thrown_ti) { (void)catch_ti; (void)thrown_ti; return 0; }

#define RET(v) (c->eax = (uint32_t)(v))
static int logging(void) { static int v = -1; if (v < 0) v = getenv("RT32_LOG_EH") != NULL; return v; }

// A type_info is {vptr, name}: the name of what a guest address is the type_info of, if it can be read.
static const char *type_name(uint32_t ti) {
    if (ti < 0x1000 || ti >= 0xE0000000u) return NULL;
    uint32_t n = rd32(ti + 4);
    return (n >= 0x1000 && n < 0xE0000000u) ? (const char *)G2H(n) : NULL;
}

static uint32_t uleb(uint32_t *p) {
    uint32_t v = 0; int sh = 0;
    for (;;) { uint8_t b = rd8((*p)++); v |= (uint32_t)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) return v; }
}
static int32_t sleb(uint32_t *p) {
    int32_t v = 0; int sh = 0; uint8_t b;
    do { b = rd8((*p)++); v |= (int32_t)(b & 0x7F) << sh; sh += 7; } while (b & 0x80);
    if (sh < 32 && (b & 0x40)) v |= -(1 << sh);
    return v;
}
static uint32_t encoded(uint32_t *p, uint8_t enc) {
    uint32_t at = *p, v;
    switch (enc & 0x0F) {
        case 0x00: case 0x03: case 0x0B: v = rd32(at); *p += 4; break;
        case 0x02: v = rd16(at); *p += 2; break;
        case 0x0A: v = (uint32_t)(int32_t)(int16_t)rd16(at); *p += 2; break;
        case 0x01: v = uleb(p); break;
        case 0x09: v = (uint32_t)sleb(p); break;
        default: v = 0; *p += 4; break;
    }
    if (v && (enc & 0x70) == 0x10) v += at;
    if (v && (enc & 0x80)) v = rd32(v);
    return v;
}

static int type_matches(uint32_t want, uint32_t thrown) {
    if (want == thrown) return 1;
    const char *w = type_name(want), *t = type_name(thrown);
    if (!w || !t) return 0;
    if (!strcmp(w, t)) return 1;
    if (w[0] == 'P' && w[1] == 'K' && t[0] == 'P' && !strcmp(w + 2, t + 1)) return 1;   // T* caught as const T*
    return eh_base_of(want, thrown);
}

// The call at a site whose landing pad's action is `action` (0: clean-up only) has come back with an exception
// in flight. 1 if the pad is to run (eax, edx set, the flag cleared), 0 if the exception goes on up.
int eh_land(CPU *c, uint32_t lsda, uint32_t action) {
    uint32_t p = lsda;
    uint8_t e = rd8(p++);
    if (e != 0xFF) encoded(&p, e);
    uint8_t tenc = rd8(p++);
    uint32_t types = 0;
    if (tenc != 0xFF) { uint32_t off = uleb(&p); types = p + off; }
    p++;                                   // the call-site encoding: the translator has read that table
    uint32_t cslen = uleb(&p), actions = p + cslen;
    int32_t sel = -1; int cleanup = action == 0;
    for (uint32_t a = action ? actions + action - 1 : 0; a;) {
        uint32_t q = a;
        int32_t filter = sleb(&q);
        uint32_t next_at = q;
        int32_t next = sleb(&q);
        if (filter == 0) cleanup = 1;
        else if (filter > 0 && types) {
            uint32_t at = types - 4u * (uint32_t)filter;
            uint32_t ti = encoded(&at, tenc);
            if (!ti || type_matches(ti, c->exc_type)) { sel = filter; break; }
        }
        a = next ? next_at + (uint32_t)next : 0;
    }
    if (sel < 0 && !cleanup) return 0;
    if (logging()) fprintf(stderr, "[eh] %s in %s (handler %d)\n", sel > 0 ? "caught" : "clean-up", rt_func_name(c->fn), sel);
    c->eax = c->exc_obj; c->edx = sel < 0 ? 0 : (uint32_t)sel; c->exc = 0;
    return 1;
}

// Nothing in the guest caught it: said by whoever called into the guest (a thread's start, a callback).
void eh_uncaught(CPU *c, const char *where) {
    const char *t = type_name(c->exc_type);
    fprintf(stderr, "[eh] a C++ exception of type %s went uncaught out of %s\n", t ? t : "?", where);
    c->exc = 0;
}

// ---- the ABI's functions ----
#define NCAUGHT 16
static _Thread_local struct { uint32_t obj, type; int rethrown; } caught[NCAUGHT];
static _Thread_local int ncaught;

void I____cxa_allocate_exception(CPU *c) { uint32_t p = eh_guest_alloc(ARG(0) ? ARG(0) : 4); memset(G2H(p), 0, ARG(0)); RET(p); }
void I____cxa_free_exception(CPU *c) { eh_guest_free(ARG(0)); }
void I____cxa_throw(CPU *c) {
    c->exc_obj = ARG(0); c->exc_type = ARG(1); c->exc_dtor = ARG(2); c->exc = 1;
    if (logging()) { const char *t = type_name(c->exc_type); fprintf(stderr, "[eh] throw of %s from %s\n", t ? t : "?", rt_func_name(rd32(c->esp))); }
}
void I____cxa_begin_catch(CPU *c) {
    if (ncaught < NCAUGHT) { caught[ncaught].obj = ARG(0); caught[ncaught].type = c->exc_type; caught[ncaught].rethrown = 0; }
    ncaught++;
    RET(ARG(0));
}
void I____cxa_end_catch(CPU *c) {
    if (ncaught <= 0) return;
    ncaught--;
    if (ncaught < NCAUGHT && !caught[ncaught].rethrown && caught[ncaught].obj) eh_guest_free(caught[ncaught].obj);
}
void I____cxa_rethrow(CPU *c) {
    if (ncaught <= 0 || ncaught > NCAUGHT) return;
    caught[ncaught - 1].rethrown = 1;
    c->exc_obj = caught[ncaught - 1].obj; c->exc_type = caught[ncaught - 1].type; c->exc = 1;
}
void I____cxa_get_exception_ptr(CPU *c) { RET(ARG(0)); }
void I___Unwind_Resume(CPU *c) { c->exc_obj = ARG(0); c->exc = 1; }
void I___Unwind_Resume_or_Rethrow(CPU *c) { c->exc_obj = ARG(0); c->exc = 1; }
void I___ZSt18uncaught_exceptionv(CPU *c) { RET(c->exc ? 1 : 0); }
