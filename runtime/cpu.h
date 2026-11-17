// Guest x86-64 CPU state and memory access for the recompiled code (host: arm64 macOS).
// Guest memory is identity-mapped: a guest address is a host address.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

// Clang vector types: packed SSE ops are emitted as whole-vector expressions on these members, so a
// cached XMM register stays in one NEON register (per-lane loops over the arrays made LLVM split it into two
// 64-bit integers and scalarize every lane).
typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef uint64_t v2q __attribute__((vector_size(16)));
typedef int64_t v2l __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint16_t v8w __attribute__((vector_size(16)));
typedef int16_t v8h __attribute__((vector_size(16)));
typedef uint8_t v16b __attribute__((vector_size(16)));
typedef int8_t v16c __attribute__((vector_size(16)));
typedef union {
    uint64_t q[2]; int64_t l[2];
    uint32_t u[4]; int32_t i[4];
    uint16_t w[8]; int16_t h[8];
    uint8_t b[16]; int8_t c[16];
    float f[4]; double d[2];
    v4f vf; v2d vd; v2q vq; v2l vl; v4u vu; v4i vi; v8w vw; v8h vh; v16b vb; v16c vc;
} XMM;
// lane select: where the (all-ones / all-zeros per lane) mask m is set take a, else b; works for any lane type
#define VSEL(m, a, b) ({ __typeof__(a) vsel_a_ = (a); __typeof__(m) vsel_m_ = (m); \
    (__typeof__(a))((vsel_m_ & (__typeof__(vsel_m_))vsel_a_) | (~vsel_m_ & (__typeof__(vsel_m_))(__typeof__(a))(b))); })
#define V4I_MIN ((v4i){INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN})

typedef struct CPU {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    XMM xmm[16];
    uint64_t gsbase;          // guest TEB of this thread
    uint32_t mxcsr;
    uint16_t fcw;             // x87 control word
    int top;                  // x87 stack top
    double st[8];             // x87 registers (double precision)
    uint64_t stack_lo, stack_hi;
    uint64_t fn;              // guest function currently executing (debug)
    void *thread;             // runtime per-thread data
    uint64_t mm[8];           // MMX registers (kept apart from the x87 stack: no game mixes the two without emms)
} CPU;
// one MMX register's lanes
typedef union { uint64_t q[1]; int64_t l[1]; uint32_t u[2]; int32_t i[2]; uint16_t w[4]; int16_t h[4]; uint8_t b[8]; int8_t c[8]; } MM64;

typedef void (*gfn)(CPU *);
#define C (*c)

// ---- memory (unaligned-safe) ----
// x86 is TSO: a thread's stores become visible in program order, and loads are not reordered with
// other loads. ARM is weaker, so lock-free guest code (job queues publishing data, then a flag) breaks
// with plain accesses. With RT_TSO (default), aligned guest loads are acquire and stores are release
// (LDAPR/STLR), which restores TSO ordering; unaligned ones are bracketed with fences.
#ifndef RT_TSO
#define RT_TSO 1
#endif
#if RT_TSO
// RT_ORDER_RELAXED (diagnostic only, not safe to ship): the same accesses as relaxed atomics, which compile to
// plain LDR/STR but still stop the compiler from caching guest memory in registers. Measures what TSO costs.
#ifdef RT_ORDER_RELAXED
#define RT_LD_ORDER __ATOMIC_RELAXED
#define RT_ST_ORDER __ATOMIC_RELAXED
#else
#define RT_LD_ORDER __ATOMIC_ACQUIRE
#define RT_ST_ORDER __ATOMIC_RELEASE
#endif
// RT_RELAX_FP: float/vector (SSE) accesses are relaxed; integer ones keep TSO. Guest code synchronizes through
// integer flags and locks, whose release/acquire still orders the surrounding float and vector data.
#ifdef RT_RELAX_FP
#define RT_FP_LD_ORDER __ATOMIC_RELAXED
#define RT_FP_ST_ORDER __ATOMIC_RELAXED
#else
#define RT_FP_LD_ORDER RT_LD_ORDER
#define RT_FP_ST_ORDER RT_ST_ORDER
#endif
#define RT_LD(T, n) static inline __attribute__((always_inline)) T rd_##n(uint64_t a) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) return __atomic_load_n((T *)(uintptr_t)a, RT_LD_ORDER); \
    T v; memcpy(&v, (void *)(uintptr_t)a, sizeof v); __atomic_thread_fence(RT_LD_ORDER); return v; }
#define RT_ST(T, n) static inline __attribute__((always_inline)) void wr_##n(uint64_t a, T v) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) { __atomic_store_n((T *)(uintptr_t)a, v, RT_ST_ORDER); return; } \
    __atomic_thread_fence(RT_ST_ORDER); memcpy((void *)(uintptr_t)a, &v, sizeof v); }
RT_LD(uint8_t, 8) RT_LD(uint16_t, 16) RT_LD(uint32_t, 32) RT_LD(uint64_t, 64)
RT_ST(uint8_t, 8) RT_ST(uint16_t, 16) RT_ST(uint32_t, 32) RT_ST(uint64_t, 64)
static inline __attribute__((always_inline)) uint8_t  rd8(uint64_t a)  { return rd_8(a); }
static inline __attribute__((always_inline)) uint16_t rd16(uint64_t a) { return rd_16(a); }
static inline __attribute__((always_inline)) uint32_t rd32(uint64_t a) { return rd_32(a); }
static inline __attribute__((always_inline)) uint64_t rd64(uint64_t a) { return rd_64(a); }
#define RT_FLD(T, n) static inline __attribute__((always_inline)) T rdo_##n(uint64_t a) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) return __atomic_load_n((T *)(uintptr_t)a, RT_FP_LD_ORDER); \
    T v; memcpy(&v, (void *)(uintptr_t)a, sizeof v); __atomic_thread_fence(RT_FP_LD_ORDER); return v; }
#define RT_FST(T, n) static inline __attribute__((always_inline)) void wro_##n(uint64_t a, T v) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) { __atomic_store_n((T *)(uintptr_t)a, v, RT_FP_ST_ORDER); return; } \
    __atomic_thread_fence(RT_FP_ST_ORDER); memcpy((void *)(uintptr_t)a, &v, sizeof v); }
RT_FLD(uint32_t, 32) RT_FLD(uint64_t, 64) RT_FST(uint32_t, 32) RT_FST(uint64_t, 64)
static inline __attribute__((always_inline)) float    rdf(uint64_t a)  { uint32_t u = rdo_32(a); float v; memcpy(&v, &u, 4); return v; }
static inline __attribute__((always_inline)) double   rdd(uint64_t a)  { uint64_t u = rdo_64(a); double v; memcpy(&v, &u, 8); return v; }
// 128-bit: two ordered 64-bit halves when 8-byte aligned (x86 doesn't promise 16-byte atomicity either),
// which avoids a full barrier (dmb) per SSE access; unaligned ones keep the fence
static inline __attribute__((always_inline)) XMM      rdx(uint64_t a)  {
    XMM v;
    if (__builtin_expect(!(a & 7), 1)) { v.q[0] = __atomic_load_n((uint64_t *)(uintptr_t)a, RT_FP_LD_ORDER); v.q[1] = __atomic_load_n((uint64_t *)(uintptr_t)(a + 8), RT_FP_LD_ORDER); return v; }
    memcpy(&v, (void *)(uintptr_t)a, 16); __atomic_thread_fence(RT_FP_LD_ORDER); return v;
}
static inline __attribute__((always_inline)) void wr8(uint64_t a, uint8_t v)   { wr_8(a, v); }
static inline __attribute__((always_inline)) void wr16(uint64_t a, uint16_t v) { wr_16(a, v); }
static inline __attribute__((always_inline)) void wr32(uint64_t a, uint32_t v) { wr_32(a, v); }
static inline __attribute__((always_inline)) void wr64(uint64_t a, uint64_t v) { wr_64(a, v); }
static inline __attribute__((always_inline)) void wrf(uint64_t a, float v)     { uint32_t u; memcpy(&u, &v, 4); wro_32(a, u); }
static inline __attribute__((always_inline)) void wrd(uint64_t a, double v)    { uint64_t u; memcpy(&u, &v, 8); wro_64(a, u); }
static inline __attribute__((always_inline)) void wrx(uint64_t a, XMM v)       {
    if (__builtin_expect(!(a & 7), 1)) { __atomic_store_n((uint64_t *)(uintptr_t)a, v.q[0], RT_FP_ST_ORDER); __atomic_store_n((uint64_t *)(uintptr_t)(a + 8), v.q[1], RT_FP_ST_ORDER); return; }
    __atomic_thread_fence(RT_FP_ST_ORDER); memcpy((void *)(uintptr_t)a, &v, 16);
}
#else
static inline __attribute__((always_inline)) uint8_t  rd8(uint64_t a)  { return *(volatile uint8_t *)(uintptr_t)a; }
static inline __attribute__((always_inline)) uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
static inline __attribute__((always_inline)) uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline __attribute__((always_inline)) uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline __attribute__((always_inline)) float    rdf(uint64_t a)  { float v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline __attribute__((always_inline)) double   rdd(uint64_t a)  { double v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline __attribute__((always_inline)) XMM      rdx(uint64_t a)  { XMM v; memcpy(&v, (void *)(uintptr_t)a, 16); return v; }
static inline __attribute__((always_inline)) void wr8(uint64_t a, uint8_t v)   { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline __attribute__((always_inline)) void wr16(uint64_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
static inline __attribute__((always_inline)) void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline __attribute__((always_inline)) void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline __attribute__((always_inline)) void wrf(uint64_t a, float v)     { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline __attribute__((always_inline)) void wrd(uint64_t a, double v)    { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline __attribute__((always_inline)) void wrx(uint64_t a, XMM v)       { memcpy((void *)(uintptr_t)a, &v, 16); }
#endif
// Stack accesses (rsp-relative, emitted by the translator as rds*/wrs*) are thread-private: plain loads/stores.
static inline __attribute__((always_inline)) uint8_t  rds8(uint64_t a)  { return *(uint8_t *)(uintptr_t)a; }
static inline __attribute__((always_inline)) uint16_t rds16(uint64_t a) { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
static inline __attribute__((always_inline)) uint32_t rds32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline __attribute__((always_inline)) uint64_t rds64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
static inline __attribute__((always_inline)) float    rdsf(uint64_t a)  { float v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline __attribute__((always_inline)) double   rdsd(uint64_t a)  { double v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
// Loads from the image's read-only sections (constants, tables, the import table; the translator decides): data
// that never changes needs no ordering, so plain loads.
#define rdc8 rds8
#define rdc16 rds16
#define rdc32 rds32
#define rdc64 rds64
#define rdcf rdsf
#define rdcd rdsd
static inline __attribute__((always_inline)) XMM rdcx(uint64_t a) { XMM v; memcpy(&v, (void *)(uintptr_t)a, 16); return v; }
static inline __attribute__((always_inline)) XMM      rdsx(uint64_t a)  { XMM v; memcpy(&v, (void *)(uintptr_t)a, 16); return v; }
static inline __attribute__((always_inline)) void wrs8(uint64_t a, uint8_t v)   { *(uint8_t *)(uintptr_t)a = v; }
static inline __attribute__((always_inline)) void wrs16(uint64_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
static inline __attribute__((always_inline)) void wrs32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline __attribute__((always_inline)) void wrs64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline __attribute__((always_inline)) void wrsf(uint64_t a, float v)     { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline __attribute__((always_inline)) void wrsd(uint64_t a, double v)    { memcpy((void *)(uintptr_t)a, &v, 8); }
static inline __attribute__((always_inline)) void wrsx(uint64_t a, XMM v)       { memcpy((void *)(uintptr_t)a, &v, 16); }
#define P8(a)  ((uint8_t *)(uintptr_t)(a))
#define P16(a) ((uint16_t *)(uintptr_t)(a))
#define P32(a) ((uint32_t *)(uintptr_t)(a))
#define P64(a) ((uint64_t *)(uintptr_t)(a))

// ---- stack ----
static inline __attribute__((always_inline)) void PUSH(CPU *c, uint64_t v) { c->rsp -= 8; wrs64(c->rsp, v); }
// register-cached functions (tools/recomp64.py cache_registers) keep rsp in the local R_rsp
#define PUSHL(v) do { R_rsp -= 8; wrs64(R_rsp, (uint64_t)(v)); } while (0)
#define POPL() ({ uint64_t v_ = rds64(R_rsp); R_rsp += 8; v_; })
static inline __attribute__((always_inline)) uint64_t POP(CPU *c) { uint64_t v = rds64(c->rsp); c->rsp += 8; return v; }

// ---- x87 ----
#define ST(i) c->st[(c->top + (i)) & 7]
static inline __attribute__((always_inline)) void FPUSH(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline __attribute__((always_inline)) void FPOP(CPU *c) { c->top = (c->top + 1) & 7; }
double rd80(uint64_t a);
void wr80(uint64_t a, double v);

// ---- helpers used by generated code ----
static inline __attribute__((always_inline)) int parity8(uint64_t v) { return !__builtin_parity((unsigned)(v & 0xFF)); }   // PF
static inline __attribute__((always_inline)) int32_t cvt_i32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v; }
static inline __attribute__((always_inline)) int64_t cvt_i64(double v) { return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v; }
double fpu_round(CPU *c, double v);
double sse_round(CPU *c, double v);           // MXCSR rounding (cvtss2si etc.)

// indirect call/jump to a guest or host address: the common case (a 16-byte-aligned function in the exe's
// .text) is one table load inline at the call site; everything else goes to call_ind_slow in rt.c
extern void (**rt_fast_tab)(CPU *); extern uint64_t rt_fast_lo, rt_fast_n;
void call_ind_slow(CPU *c, uint64_t target);
static inline __attribute__((always_inline)) void call_ind(CPU *c, uint64_t t) {
    uint64_t off = t - rt_fast_lo;
    if (__builtin_expect(!(off & 15) && (off >> 4) < rt_fast_n, 1)) {
        void (*f)(CPU *) = rt_fast_tab[off >> 4];
        if (__builtin_expect(f != 0, 1)) { f(c); return; }
    }
    call_ind_slow(c, t);
}
void bad_insn(CPU *c, uint64_t addr, const char *what);
void bad_jump(CPU *c, uint64_t target, uint64_t from);
void missing_func(CPU *c, uint64_t target, uint64_t from);
void cpu_cpuid(CPU *c);
void cpu_syscall_trap(CPU *c, uint64_t addr);
