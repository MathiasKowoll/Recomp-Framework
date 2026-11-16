// Native replacements for hot guest library routines (see tools/native.txt for the addresses).
// The translator emits `void F_<addr>(CPU *c) { N_x(c); c->rsp += 8; }` in place of the guest code,
// so every direct call, tail jump and indirect call to that address runs this instead.
// Guest memory is identity-mapped, so guest pointers are host pointers.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cpu.h"
#include "rt.h"

// RT_NATIVE_OFF=all or a comma list of guest addresses (hex): those replacements are switched off and the
// translated guest code runs instead (the translator keeps it as F_<addr>__orig). For A/B testing.
int rt_native_enabled(uint64_t a) {
    const char *e = getenv("RT_NATIVE_OFF");
    if (!e || !*e) return 1;
    if (!strcmp(e, "all")) { rt_log("[native] 0x%llx: using the translated code\n", (unsigned long long)a); return 0; }
    for (const char *p = e; *p; ) {
        char *end; unsigned long long v = strtoull(p, &end, 16);
        if (end == p) break;
        if (v == a) { rt_log("[native] 0x%llx: using the translated code\n", (unsigned long long)a); return 0; }
        p = *end ? end + 1 : end;
    }
    return 1;
}

// ---- MSVC C runtime (statically linked into the exe) ----
void N_memmove(CPU *c) { memmove(PTR(A0), PTR(A1), (size_t)A2); RET(A0); }
void N_memset(CPU *c)  { memset(PTR(A0), (uint8_t)A1, (size_t)A2); RET(A0); }
void N_strlen(CPU *c)  { RET(strlen(PTR(A0))); }

// ---- zlib 1.2.3 inflate_fast(z_streamp strm, unsigned start) ----
// Win64 layouts (unsigned long is 32-bit there). A straight port of zlib 1.2.3 inffast.c.
typedef struct {
    uint64_t next_in; uint32_t avail_in, total_in;
    uint64_t next_out; uint32_t avail_out, total_out;
    uint64_t msg, state;
} GZStream;
typedef struct {
    uint32_t mode; int32_t last, wrap, havedict, flags; uint32_t dmax, check, total;
    uint64_t head;
    uint32_t wbits, wsize, whave, write;
    uint64_t window;
    uint32_t hold, bits, length, offset, extra, pad_;
    uint64_t lencode, distcode;
    uint32_t lenbits, distbits;
} GInflateState;
typedef struct { uint8_t op, bits; uint16_t val; } ZCode;
_Static_assert(__builtin_offsetof(GZStream, state) == 0x28, "z_stream");
_Static_assert(__builtin_offsetof(GInflateState, hold) == 0x40, "hold");
_Static_assert(__builtin_offsetof(GInflateState, lencode) == 0x58, "lencode");
_Static_assert(__builtin_offsetof(GInflateState, lenbits) == 0x68, "lenbits");
enum { Z_TYPE = 11, Z_BAD = 27 };
// the guest's own message strings (so strm->msg points into the image, as before)
// strm->msg: guest memory is identity-mapped, so the guest reads these host strings directly
#define ZMSG_LITLEN ((uint64_t)(uintptr_t)"invalid literal/length code")
#define ZMSG_DIST   ((uint64_t)(uintptr_t)"invalid distance code")
#define ZMSG_FAR    ((uint64_t)(uintptr_t)"invalid distance too far back")

void N_inflate_fast(CPU *c) {
    GZStream *strm = PTR(A0);
    unsigned start = (unsigned)A1;
    GInflateState *state = PTR(strm->state);
    const uint8_t *in = PTR(strm->next_in);
    const uint8_t *last = in + (strm->avail_in - 5);
    uint8_t *out = PTR(strm->next_out);
    uint8_t *beg = out - (start - strm->avail_out);
    uint8_t *end = out + (strm->avail_out - 257);
    unsigned wsize = state->wsize, whave = state->whave, write = state->write;
    const uint8_t *window = PTR(state->window);
    uint32_t hold = state->hold; unsigned bits = state->bits;
    const ZCode *lcode = PTR(state->lencode), *dcode = PTR(state->distcode);
    unsigned lmask = (1U << state->lenbits) - 1, dmask = (1U << state->distbits) - 1;
    ZCode this;
    unsigned op, len, dist;
    const uint8_t *from;

    do {
        if (bits < 15) {
            hold += (uint32_t)(*in++) << bits; bits += 8;
            hold += (uint32_t)(*in++) << bits; bits += 8;
        }
        this = lcode[hold & lmask];
      dolen:
        op = this.bits; hold >>= op; bits -= op;
        op = this.op;
        if (op == 0) {
            *out++ = (uint8_t)this.val;
        } else if (op & 16) {
            len = this.val;
            op &= 15;
            if (op) {
                if (bits < op) { hold += (uint32_t)(*in++) << bits; bits += 8; }
                len += hold & ((1U << op) - 1);
                hold >>= op; bits -= op;
            }
            if (bits < 15) {
                hold += (uint32_t)(*in++) << bits; bits += 8;
                hold += (uint32_t)(*in++) << bits; bits += 8;
            }
            this = dcode[hold & dmask];
          dodist:
            op = this.bits; hold >>= op; bits -= op;
            op = this.op;
            if (op & 16) {
                dist = this.val;
                op &= 15;
                if (bits < op) {
                    hold += (uint32_t)(*in++) << bits; bits += 8;
                    if (bits < op) { hold += (uint32_t)(*in++) << bits; bits += 8; }
                }
                dist += hold & ((1U << op) - 1);
                hold >>= op; bits -= op;
                op = (unsigned)(out - beg);
                if (dist > op) {                       // copy from the window
                    op = dist - op;
                    if (op > whave) { strm->msg = ZMSG_FAR; state->mode = Z_BAD; break; }
                    from = window;
                    if (write == 0) {
                        from += wsize - op;
                        if (op < len) { len -= op; do { *out++ = *from++; } while (--op); from = out - dist; }
                    } else if (write < op) {
                        from += wsize + write - op;
                        op -= write;
                        if (op < len) {
                            len -= op; do { *out++ = *from++; } while (--op);
                            from = window;
                            if (write < len) { op = write; len -= op; do { *out++ = *from++; } while (--op); from = out - dist; }
                        }
                    } else {
                        from += write - op;
                        if (op < len) { len -= op; do { *out++ = *from++; } while (--op); from = out - dist; }
                    }
                    while (len > 2) { out[0] = from[0]; out[1] = from[1]; out[2] = from[2]; out += 3; from += 3; len -= 3; }
                    if (len) { *out++ = *from++; if (len > 1) *out++ = *from++; }
                } else {                               // copy from the output (may overlap: byte order matters)
                    from = out - dist;
                    do { out[0] = from[0]; out[1] = from[1]; out[2] = from[2]; out += 3; from += 3; len -= 3; } while (len > 2);
                    if (len) { *out++ = *from++; if (len > 1) *out++ = *from++; }
                }
            } else if ((op & 64) == 0) {
                this = dcode[this.val + (hold & ((1U << op) - 1))];
                goto dodist;
            } else { strm->msg = ZMSG_DIST; state->mode = Z_BAD; break; }
        } else if ((op & 64) == 0) {
            this = lcode[this.val + (hold & ((1U << op) - 1))];
            goto dolen;
        } else if (op & 32) {
            state->mode = Z_TYPE; break;
        } else { strm->msg = ZMSG_LITLEN; state->mode = Z_BAD; break; }
    } while (in < last && out < end);

    len = bits >> 3; in -= len; bits -= len << 3;
    hold &= (1U << bits) - 1;
    strm->next_in = G(in);
    strm->next_out = G(out);
    strm->avail_in = (unsigned)(in < last ? 5 + (last - in) : 5 - (in - last));
    strm->avail_out = (unsigned)(out < end ? 257 + (end - out) : 257 - (out - end));
    state->hold = hold;
    state->bits = bits;
}

// ---- debugging: write-protect a guest memory range (rt_watch_set + rt_watch(1)); any write to it faults and the
// crash handler names the guest function. rt_watch(0) opens it again (around the owner's own writes). ----
#include <sys/mman.h>
#include <unistd.h>
static uint64_t watch_lo, watch_hi;
void rt_watch_set(uint64_t addr, uint64_t len) {
    uint64_t pg = (uint64_t)getpagesize();
    watch_lo = (addr + pg - 1) & ~(pg - 1); watch_hi = (addr + len) & ~(pg - 1);   // whole pages inside the range only
    if (watch_hi <= watch_lo) watch_lo = watch_hi = 0;
    rt_log("[watch] 0x%llx..0x%llx\n", (unsigned long long)watch_lo, (unsigned long long)watch_hi);
}
void rt_watch(int on) {
    if (watch_hi > watch_lo) mprotect((void *)watch_lo, watch_hi - watch_lo, on ? PROT_READ : PROT_READ | PROT_WRITE);
}
void rt_watch_clear(void) { rt_watch(0); watch_lo = watch_hi = 0; }
