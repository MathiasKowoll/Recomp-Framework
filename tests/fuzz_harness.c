// Instruction fuzzer harness (see tests/insn_fuzz.py). Built twice: -arch x86_64 -DX86 runs the original
// instruction bytes under Rosetta; -arch arm64 runs their translation. Both read build/fuzz/inputs.bin and
// write one record per trial: registers, XMM registers, and up to 64 changed bytes of the data buffer.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define BUF 0x2a0000000ull
#define BUF_SIZE (1u << 20)
#define WIN (64u << 10)              // the randomized window around the middle of the buffer
#define CODE_BASE 0x150000000ull
typedef struct { uint64_t g[16]; uint8_t x[256]; } State;
typedef struct { uint64_t g[16]; uint8_t x[256]; uint64_t pad; uint32_t n; struct { uint32_t off; uint8_t v; } __attribute__((packed)) ch[64]; } __attribute__((packed)) Rec;
static uint8_t *buf, ref[WIN];
static void fill(uint32_t seed) {
    uint32_t s = seed | 1;
    for (uint32_t i = 0; i < WIN; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; ref[i] = (uint8_t)s; }
    memcpy(buf + BUF_SIZE / 2 - WIN / 2, ref, WIN);
}
static void diffs(Rec *r) {
    r->n = 0;
    uint8_t *w = buf + BUF_SIZE / 2 - WIN / 2;
    for (uint32_t i = 0; i < WIN && r->n < 64; i++) if (w[i] != ref[i]) { r->ch[r->n].off = i; r->ch[r->n].v = w[i]; r->n++; }
    // bytes outside the window should stay zero
    for (uint32_t i = 0; i < BUF_SIZE / 2 - WIN / 2 && r->n < 64; i += 1) {
        if (buf[i] && r->n < 64) { r->ch[r->n].off = 0x80000000u | i; r->ch[r->n].v = buf[i]; r->n++; buf[i] = 0; }
        if (buf[BUF_SIZE / 2 + WIN / 2 + i] && r->n < 64) { r->ch[r->n].off = 0x40000000u | i; r->ch[r->n].v = buf[BUF_SIZE / 2 + WIN / 2 + i]; r->n++; buf[BUF_SIZE / 2 + WIN / 2 + i] = 0; }
    }
}
#ifdef X86
void run_snippet(State *s, void *code);
#else
#include "cpu.h"
typedef void (*fn_t)(CPU *);
extern const fn_t fuzz_fns[]; extern const int fuzz_n;
// runtime pieces the generated code may reference
void (**rt_fast_tab)(CPU *); uint64_t rt_fast_lo, rt_fast_n;
void call_ind_slow(CPU *c, uint64_t t) { fprintf(stderr, "call_ind\n"); exit(2); }
void bad_insn(CPU *c, uint64_t a, const char *w) { c->rax = 0xBAD1BAD1BAD1BAD1ull; }
void bad_jump(CPU *c, uint64_t t, uint64_t f) { exit(3); }
void missing_func(CPU *c, uint64_t t, uint64_t f) { exit(4); }
void cpu_cpuid(CPU *c) { }
void cpu_syscall_trap(CPU *c, uint64_t a) { exit(5); }
void unimpl_import(CPU *c, const char *n) { exit(6); }
double sse_round(CPU *c, double v) { return __builtin_nearbyint(v); }
double fpu_round(CPU *c, double v) { return __builtin_nearbyint(v); }
double rd80(uint64_t a) { return 0; }
void wr80(uint64_t a, double v) { }
#endif
#include <signal.h>
#include <unistd.h>
#include <setjmp.h>
static volatile uint32_t cur_t;
static sigjmp_buf jb;
static void on_fault(int sig) { siglongjmp(jb, sig); }   // a privileged or garbage instruction: skip the trial
int main(int argc, char **argv) {
    char p[1024];
    signal(SIGSEGV, on_fault); signal(SIGBUS, on_fault); signal(SIGILL, on_fault); signal(SIGFPE, on_fault);
    buf = mmap((void *)BUF, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (buf != (void *)BUF) { perror("buf"); return 1; }
    snprintf(p, sizeof p, "%s/inputs.bin", argv[1]);
    FILE *in = fopen(p, "rb"); uint32_t nt, ntr, slot; fread(&nt, 4, 1, in); fread(&ntr, 4, 1, in); fread(&slot, 4, 1, in);
#ifdef X86
    snprintf(p, sizeof p, "%s/code.bin", argv[1]);
    FILE *cf = fopen(p, "rb"); fseek(cf, 0, SEEK_END); long cl = ftell(cf); fseek(cf, 0, SEEK_SET);
    uint8_t *code = mmap((void *)CODE_BASE, (size_t)cl + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    fread(code, 1, (size_t)cl, cf); fclose(cf);
#else
    static uint8_t stack[1 << 16] __attribute__((aligned(16)));
#endif
    Rec r;
    for (uint32_t t = 0; t < nt; t++)
        for (uint32_t k = 0; k < ntr; k++) {
            State s; uint32_t seed;
            fread(s.g, 8, 16, in); fread(s.x, 1, 256, in); fread(&seed, 4, 1, in);
            fill(seed); cur_t = t;
            memset(&r, 0, sizeof r);
            if (sigsetjmp(jb, 1)) { r.n = 0xFFFFFFFFu; fwrite(&r, sizeof r, 1, stdout); continue; }
#ifdef X86
            run_snippet(&s, (void *)(CODE_BASE + (uint64_t)t * slot));
#else
            static CPU c; memset(&c, 0, sizeof c);
            uint64_t *gp[16] = { &c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi, &c.r8, &c.r9, &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15 };
            for (int i = 0; i < 16; i++) *gp[i] = s.g[i];
            memcpy(c.xmm, s.x, 256);
            c.mxcsr = 0x1F80;
            c.rsp = (uint64_t)(stack + sizeof stack - 64); c.rsp -= 8; *(uint64_t *)c.rsp = 0x1234;
            fuzz_fns[t](&c);
            for (int i = 0; i < 16; i++) s.g[i] = *gp[i];
            memcpy(s.x, c.xmm, 256);
#endif
            memset(&r, 0, sizeof r);
            memcpy(r.g, s.g, sizeof s.g); r.g[4] = 0; memcpy(r.x, s.x, 256);
            diffs(&r);
            fwrite(&r, sizeof r, 1, stdout);
        }
    return 0;
}
