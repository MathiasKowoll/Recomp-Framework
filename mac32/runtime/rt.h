// Runtime for statically recompiled i386 code.
// Guest memory is a 4 GiB reservation; guest address a lives at M + a.
// Every translated function / shim receives `CPU *c` (one CPU per guest thread);
// inside them `C` names the current CPU.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <setjmp.h>

typedef union {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    uint16_t w[8];
    uint8_t b[16];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    XMM xmm[8];
    uint64_t mm[8];
    double st[8];   // x87 stack, st[top] = st(0); kept at double precision
    int top;
    uint16_t fcw;   // x87 control word (rounding mode used by fistp)
    uint32_t stack_lo, stack_hi;
    uint32_t fn;    // guest function currently executing (debug)
    uint32_t mxcsr; // SSE control and status: its rounding mode is given to the host, the rest is only kept
} CPU;

typedef void (*gfn)(CPU *c);
extern uint8_t rt_watch[];
void rt_on_watch(struct CPU *c, uint32_t a);
#ifndef NO_TRACE
#define TRACE_FN(a) do { c->fn = (a); if (__builtin_expect(rt_watch[(a) >> 3] & (1u << ((a) & 7)), 0)) rt_on_watch(c, (a)); } while (0)
#else
#define TRACE_FN(a) ((void)0)
#endif
#define C (*c)

extern uint8_t *M;

static inline uint8_t  rd8 (uint32_t a) { return M[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline float    rdf (uint32_t a) { float v;    memcpy(&v, M + a, 4); return v; }
static inline double   rdd (uint32_t a) { double v;   memcpy(&v, M + a, 8); return v; }
#ifdef MEMCHECK
extern uint32_t rt_mc_lo, rt_mc_hi;
void rt_mc_hit(uint32_t a, uint32_t n);
#define MC(a, n) do { if (__builtin_expect((a) < rt_mc_hi && (a) + (n) > rt_mc_lo, 0)) rt_mc_hit((a), (n)); } while (0)
#else
#define MC(a, n) ((void)0)
#endif
static inline void wr8 (uint32_t a, uint8_t v)  { MC(a, 1); M[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { MC(a, 2); memcpy(M + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { MC(a, 4); memcpy(M + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { MC(a, 8); memcpy(M + a, &v, 8); }
static inline void wrf (uint32_t a, float v)    { memcpy(M + a, &v, 4); }
static inline void wrd (uint32_t a, double v)   { memcpy(M + a, &v, 8); }
static inline XMM rdx(uint32_t a) { XMM v; memcpy(&v, M + a, 16); return v; }
static inline void wrx(uint32_t a, XMM v) { MC(a, 16); memcpy(M + a, &v, 16); }

static inline void cpu_push(CPU *c, uint32_t v) { c->esp -= 4; wr32(c->esp, v); }
static inline uint32_t cpu_pop(CPU *c) { uint32_t v = rd32(c->esp); c->esp += 4; return v; }
#define PUSH(v) cpu_push(c, (v))
#define POP() cpu_pop(c)

// x87 (only what gcc emits for float returns/conversions)
static inline void cpu_fpush(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double cpu_fpop(CPU *c) { double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
#define FPUSH(v) cpu_fpush(c, (v))
#define FPOP() cpu_fpop(c)
#define ST(i) c->st[(c->top + (i)) & 7]
double fpu_round(CPU *c, double v);   // honours fcw rounding control

// guest pointer <-> host pointer
#define G2H(a) ((void *)(M + (uint32_t)(a)))
#define H2G(p) ((uint32_t)((uint8_t *)(p) - M))

// argument i of a cdecl import shim (return address is at esp)
#define ARG(i) rd32(c->esp + 4 + 4 * (i))
#define ARGD(slot) rdd(c->esp + 4 + 4 * (slot))
#define ARGF(slot) rdf(c->esp + 4 + 4 * (slot))

void call_ind(CPU *c, uint32_t target);   // call (return address already pushed)
void missing_func(CPU *c, uint32_t addr, uint32_t from);
void bad_jump(CPU *c, uint32_t target, uint32_t from);
void bad_insn(CPU *c, uint32_t addr);
int32_t cvtt_i32(double v);
void cpu_set_mxcsr(CPU *c, uint32_t v);
// to an integer in the current rounding mode; out of range and NaN give the processor's "integer indefinite"
static inline int32_t cvtr_i32(double v) { return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)lrint(v) : INT32_MIN; }
static inline int64_t cvtt_i64(double v) { return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN; }
// rcpps and rsqrtps. The processor's are approximations to 12 bits and these are exact, but its edges are kept:
// a denormal operand counts as zero, and a result too small for a normal float is zero.
static inline float rcp_f(float v) {
    if (fabsf(v) < FLT_MIN) return copysignf(INFINITY, v);
    float r = 1.0f / v;
    return fabsf(r) < FLT_MIN ? copysignf(0.0f, r) : r;
}
static inline float rsqrt_f(float v) { return fabsf(v) < FLT_MIN ? copysignf(INFINITY, v) : 1.0f / sqrtf(v); }
// The ten-byte extended format of fld and fstp: 64 bits of mantissa with its leading one written out, then the
// sign and 15 bits of exponent. The x87 stack is kept at double precision, so the last 11 bits are rounded away.
static inline double rdt(uint32_t a) {
    uint64_t m = rd64(a); uint16_t se = rd16(a + 8);
    int e = se & 0x7FFF;
    double v = e == 0x7FFF ? ((m << 1) ? NAN : INFINITY) : ldexp((double)m, (e ? e : 1) - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static inline void wrt(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0; uint64_t m = 0;
    v = fabs(v);
    if (isnan(v)) { m = 3ull << 62; se |= 0x7FFF; }
    else if (isinf(v)) { m = 1ull << 63; se |= 0x7FFF; }
    else if (v != 0) { int x; double f = frexp(v, &x); m = (uint64_t)ldexp(f, 64); se |= (uint16_t)(x - 1 + 16383); }
    wr64(a, m); wr16(a + 8, se);
}
void cpu_cpuid(CPU *c);
const char *rt_func_name(uint32_t addr);
extern _Thread_local CPU *rt_cur;

// setjmp/longjmp support: translated code calls setjmp inline in its own frame
typedef struct {
    uint32_t guest_buf, esp, ebp, ebx, esi, edi, val;
    CPU *cpu;
    jmp_buf host;
} GJmp;
GJmp *gjmp_slot(uint32_t guest_buf);
static inline void gjmp_save(CPU *c, GJmp *j) {
    j->cpu = c; j->esp = c->esp; j->ebp = c->ebp; j->ebx = c->ebx; j->esi = c->esi; j->edi = c->edi;
}
static inline void gjmp_restore(CPU *c, GJmp *j) {
    c->esp = j->esp; c->ebp = j->ebp; c->ebx = j->ebx; c->esi = j->esi; c->edi = j->edi;
    c->eax = j->val;
}

static inline uint32_t rol32(uint32_t v, unsigned n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
