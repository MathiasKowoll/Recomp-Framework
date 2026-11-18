// Guest i386 CPU state and memory access for recompiled 32-bit Windows code (host: arm64 macOS).
// Guest memory is a 4 GiB window: guest address a lives at host GUEST_BASE + a. arm64 macOS keeps the low 4 GiB
// of every process unmapped (__PAGEZERO), so the image can't sit at its own address as on the x86-64 target.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifndef GUEST_BASE
#define GUEST_BASE 0x10000000000ull   // 1 TiB: far from the main image, dyld's shared cache and malloc zones
#endif
#define G2H(a) ((void *)(uintptr_t)(GUEST_BASE + (uint32_t)(a)))
#define H2G(p) ((uint32_t)((uintptr_t)(p) - GUEST_BASE))

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
#define VSEL(m, a, b) ({ __typeof__(a) vsel_a_ = (a); __typeof__(m) vsel_m_ = (m); \
    (__typeof__(a))((vsel_m_ & (__typeof__(vsel_m_))vsel_a_) | (~vsel_m_ & (__typeof__(vsel_m_))(__typeof__(a))(b))); })
#define V4I_MIN ((v4i){INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN})
typedef union { uint64_t q[1]; int64_t l[1]; uint32_t u[2]; int32_t i[2]; uint16_t w[4]; int16_t h[4]; uint8_t b[8]; int8_t c[8]; } MM64;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    XMM xmm[8];
    uint32_t fsbase;          // guest TEB of this thread (fs:[0] = SEH chain, fs:[0x18] = TEB itself)
    uint32_t mxcsr;
    uint16_t fcw, fsw;        // x87 control / status words
    int top;                  // x87 stack top
    double st[8];             // x87 registers (double precision)
    uint32_t stack_lo, stack_hi;
    uint32_t fn;              // guest function currently executing (debug)
    void *thread;             // runtime per-thread data
    uint64_t mm[8];           // MMX registers
} CPU;

typedef void (*gfn)(CPU *);
#define C (*c)

// ---- memory ----
// x86 is TSO; with RT_TSO (default) aligned guest loads are acquire and stores release (see runtime/cpu.h).
#ifndef RT_TSO
#define RT_TSO 1
#endif
#define HA(a) ((uintptr_t)(GUEST_BASE + (uint32_t)(a)))
#if RT_TSO
#ifdef RT_ORDER_RELAXED
#define RT_LD_ORDER __ATOMIC_RELAXED
#define RT_ST_ORDER __ATOMIC_RELAXED
#else
#define RT_LD_ORDER __ATOMIC_ACQUIRE
#define RT_ST_ORDER __ATOMIC_RELEASE
#endif
#ifdef RT_RELAX_FP
#define RT_FP_LD_ORDER __ATOMIC_RELAXED
#define RT_FP_ST_ORDER __ATOMIC_RELAXED
#else
#define RT_FP_LD_ORDER RT_LD_ORDER
#define RT_FP_ST_ORDER RT_ST_ORDER
#endif
#define RT_LD(T, n, O) static inline __attribute__((always_inline)) T rd_##n(uint32_t a) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) return __atomic_load_n((T *)HA(a), O); \
    T v; memcpy(&v, (void *)HA(a), sizeof v); __atomic_thread_fence(O); return v; }
#define RT_ST(T, n, O) static inline __attribute__((always_inline)) void wr_##n(uint32_t a, T v) { \
    if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) { __atomic_store_n((T *)HA(a), v, O); return; } \
    __atomic_thread_fence(O); memcpy((void *)HA(a), &v, sizeof v); }
RT_LD(uint8_t, 8, RT_LD_ORDER) RT_LD(uint16_t, 16, RT_LD_ORDER) RT_LD(uint32_t, 32, RT_LD_ORDER) RT_LD(uint64_t, 64, RT_LD_ORDER)
RT_ST(uint8_t, 8, RT_ST_ORDER) RT_ST(uint16_t, 16, RT_ST_ORDER) RT_ST(uint32_t, 32, RT_ST_ORDER) RT_ST(uint64_t, 64, RT_ST_ORDER)
RT_LD(uint32_t, f32, RT_FP_LD_ORDER) RT_LD(uint64_t, f64, RT_FP_LD_ORDER)
RT_ST(uint32_t, f32, RT_FP_ST_ORDER) RT_ST(uint64_t, f64, RT_FP_ST_ORDER)
#define rd8 rd_8
#define rd16 rd_16
#define rd32 rd_32
#define rd64 rd_64
#define wr8 wr_8
#define wr16 wr_16
#define wr32 wr_32
#define wr64 wr_64
static inline __attribute__((always_inline)) float  rdf(uint32_t a) { uint32_t u = rd_f32(a); float v; memcpy(&v, &u, 4); return v; }
static inline __attribute__((always_inline)) double rdd(uint32_t a) { uint64_t u = rd_f64(a); double v; memcpy(&v, &u, 8); return v; }
static inline __attribute__((always_inline)) void wrf(uint32_t a, float v)  { uint32_t u; memcpy(&u, &v, 4); wr_f32(a, u); }
static inline __attribute__((always_inline)) void wrd(uint32_t a, double v) { uint64_t u; memcpy(&u, &v, 8); wr_f64(a, u); }
static inline __attribute__((always_inline)) XMM rdx(uint32_t a) {
    XMM v;
    if (__builtin_expect(!(a & 7), 1)) { v.q[0] = __atomic_load_n((uint64_t *)HA(a), RT_FP_LD_ORDER); v.q[1] = __atomic_load_n((uint64_t *)HA(a + 8), RT_FP_LD_ORDER); return v; }
    memcpy(&v, (void *)HA(a), 16); __atomic_thread_fence(RT_FP_LD_ORDER); return v;
}
static inline __attribute__((always_inline)) void wrx(uint32_t a, XMM v) {
    if (__builtin_expect(!(a & 7), 1)) { __atomic_store_n((uint64_t *)HA(a), v.q[0], RT_FP_ST_ORDER); __atomic_store_n((uint64_t *)HA(a + 8), v.q[1], RT_FP_ST_ORDER); return; }
    __atomic_thread_fence(RT_FP_ST_ORDER); memcpy((void *)HA(a), &v, 16);
}
#else
#define RT_PLAIN(T, n) static inline __attribute__((always_inline)) T rd##n(uint32_t a) { T v; memcpy(&v, (void *)HA(a), sizeof v); return v; } \
    static inline __attribute__((always_inline)) void wr##n(uint32_t a, T v) { memcpy((void *)HA(a), &v, sizeof v); }
RT_PLAIN(uint8_t, 8) RT_PLAIN(uint16_t, 16) RT_PLAIN(uint32_t, 32) RT_PLAIN(uint64_t, 64) RT_PLAIN(float, f) RT_PLAIN(double, d) RT_PLAIN(XMM, x)
#endif
// stack (esp-relative) and read-only-section accesses: plain loads/stores
#define RT_PLAIN2(T, n) static inline __attribute__((always_inline)) T rds##n(uint32_t a) { T v; memcpy(&v, (void *)HA(a), sizeof v); return v; } \
    static inline __attribute__((always_inline)) void wrs##n(uint32_t a, T v) { memcpy((void *)HA(a), &v, sizeof v); }
RT_PLAIN2(uint8_t, 8) RT_PLAIN2(uint16_t, 16) RT_PLAIN2(uint32_t, 32) RT_PLAIN2(uint64_t, 64) RT_PLAIN2(float, f) RT_PLAIN2(double, d) RT_PLAIN2(XMM, x)
#define rdc8 rds8
#define rdc16 rds16
#define rdc32 rds32
#define rdc64 rds64
#define rdcf rdsf
#define rdcd rdsd
#define rdcx rdsx

// ---- stack ----
static inline __attribute__((always_inline)) void PUSH(CPU *c, uint32_t v) { c->esp -= 4; wrs32(c->esp, v); }
static inline __attribute__((always_inline)) uint32_t POP(CPU *c) { uint32_t v = rds32(c->esp); c->esp += 4; return v; }
// register-cached functions keep esp in R_esp; the value is evaluated before esp moves (push esp, push [esp+4])
#define PUSHL(v) do { uint32_t pushl_v_ = (uint32_t)(v); R_esp -= 4; wrs32(R_esp, pushl_v_); } while (0)
#define POPL() ({ uint32_t popl_v_ = rds32(R_esp); R_esp += 4; popl_v_; })

// ---- x87 ----
#define ST(i) c->st[(c->top + (i)) & 7]
static inline __attribute__((always_inline)) void FPUSH(CPU *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline __attribute__((always_inline)) void FPOP(CPU *c) { c->top = (c->top + 1) & 7; }
double rd80(uint32_t a);
void wr80(uint32_t a, double v);

// ---- helpers used by generated code ----
static inline __attribute__((always_inline)) int parity8(uint32_t v) { return !__builtin_parity((unsigned)(v & 0xFF)); }
static inline __attribute__((always_inline)) int32_t cvt_i32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v; }
static inline __attribute__((always_inline)) int64_t cvt_i64(double v) { return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v; }
double fpu_round(CPU *c, double v);
double sse_round(CPU *c, double v);

// indirect calls: guest functions are 16-byte aligned (MSVC), so a direct-mapped table over .text resolves most
// targets with one load; the rest (unaligned starts, import thunks, host callbacks) go to call_ind_slow
extern gfn *rt_fast_tab; extern uint32_t rt_fast_lo, rt_fast_n;
void call_ind_slow(CPU *c, uint32_t target);
static inline __attribute__((always_inline)) void call_ind(CPU *c, uint32_t t) {
    uint32_t off = t - rt_fast_lo;
    if (__builtin_expect(!(off & 15) && (off >> 4) < rt_fast_n, 1)) {
        gfn f = rt_fast_tab[off >> 4];
        if (__builtin_expect(f != 0, 1)) { f(c); return; }
    }
    call_ind_slow(c, t);
}
void bad_insn(CPU *c, uint32_t addr, const char *what);
void bad_jump(CPU *c, uint32_t target, uint32_t from);
void missing_func(CPU *c, uint32_t target, uint32_t from);
void cpu_cpuid(CPU *c);
void cpu_int(CPU *c, uint32_t addr, int n);
void unimpl_import(CPU *c, const char *name);
