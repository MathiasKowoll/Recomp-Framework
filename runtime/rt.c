// Runtime core: guest image, dispatch, host-function thunks, CPU helpers, guest calls.
#include "rt.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <fenv.h>
#include <spawn.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <crt_externs.h>
#include <unistd.h>

int rt_strict;   // RT_STRICT=1: abort on unimplemented imports

// ---------------- logging ----------------
// RT_LOGTIME=1: prefix lines with seconds since start (for timing startup phases)
void rt_log(const char *fmt, ...) {
    static int timed = -1; static uint64_t t0;
    if (timed < 0) { timed = getenv("RT_LOGTIME") != NULL; t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
    if (timed) fprintf(stderr, "%8.3f ", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

// ---------------- host functions callable from guest code ----------------
// Each native function gets a synthetic guest address in a reserved, inaccessible range,
// so it can be stored in function pointers / the IAT and reached through call_ind.
#define NHOST 8192
static struct { gfn fn; const char *name; } hostfns[NHOST];
static int nhost;
static pthread_mutex_t host_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t rt_host_fn(gfn f, const char *name) {
    pthread_mutex_lock(&host_mu);
    for (int i = 0; i < nhost; i++) if (hostfns[i].fn == f) { pthread_mutex_unlock(&host_mu); return HOSTFN_BASE + 16ull * i; }
    if (nhost == NHOST) { rt_log("[rt] host function table full\n"); abort(); }
    int i = nhost++;
    hostfns[i].fn = f; hostfns[i].name = name;
    pthread_mutex_unlock(&host_mu);
    return HOSTFN_BASE + 16ull * i;
}
const char *rt_host_name(uint64_t a) {
    if (a >= HOSTFN_BASE && a < HOSTFN_BASE + 16ull * nhost) return hostfns[(a - HOSTFN_BASE) / 16].name;
    return NULL;
}

// ---------------- guest function lookup / dispatch ----------------
static gfn find_in(const GFunc *f, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f[m].addr == a) return f[m].fn;
        if (f[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}
// translated DLLs (registered by their generated tables.c at startup)
#define MAXMOD 8
static const GModule *modules[MAXMOD];
static int nmodules, module_loaded[MAXMOD];
void rt_register_module(const GModule *m) { if (nmodules < MAXMOD) modules[nmodules++] = m; }
// Direct lookup for 16-byte-aligned function addresses (nearly all of them): one load instead of a
// binary search over ~144k entries on every indirect call. Read-only after startup.
gfn *rt_fast_tab; uint64_t rt_fast_lo, rt_fast_n;   // covers .text only (~16 MB)
__attribute__((constructor(102))) static void fast_init(void) {
    for (int i = 0; i < ngsections; i++)
        if (!strcmp(gsections[i].name, ".text")) { rt_fast_lo = gsections[i].va; rt_fast_n = (gsections[i].vsize + 15) / 16; }
    if (!rt_fast_n) return;
    rt_fast_tab = calloc(rt_fast_n, sizeof(gfn));
    for (int i = 0; i < ngfuncs; i++) {
        uint64_t off = gfuncs[i].addr - rt_fast_lo;
        if (gfuncs[i].addr >= rt_fast_lo && !(off & 15) && off / 16 < rt_fast_n) rt_fast_tab[off / 16] = gfuncs[i].fn;
    }
}
gfn rt_find_func(uint64_t a) {
    if (a >= gimage_base && a < gimage_base + gimage_size) {
        uint64_t off = a - rt_fast_lo;
        if (rt_fast_tab && !(off & 15) && off / 16 < rt_fast_n) { gfn f = rt_fast_tab[off / 16]; if (f) return f; }
        return find_in(gfuncs, ngfuncs, a);
    }
    for (int i = 0; i < nmodules; i++)
        if (module_loaded[i] && a >= modules[i]->base && a < modules[i]->base + modules[i]->size) return find_in(modules[i]->funcs, modules[i]->nfuncs, a);
    return NULL;
}

void call_ind_slow(CPU *c, uint64_t t) {
    if (t >= HOSTFN_BASE && t < HOSTFN_BASE + 16ull * nhost) {
        // native function: cdecl-like, the caller's return address is still on the guest stack
        hostfns[(t - HOSTFN_BASE) / 16].fn(c);
        c->rsp += 8;
        return;
    }
    gfn f = rt_find_func(t);
    if (f) { f(c); return; }
    rt_log("[rt] indirect call to unknown address 0x%llx (from function 0x%llx, return 0x%llx)\n",
           (unsigned long long)t, (unsigned long long)c->fn, (unsigned long long)rd64(c->rsp));
    rt_backtrace(c);
    abort();
}

void bad_insn(CPU *c, uint64_t a, const char *what) {
    rt_log("[rt] unsupported instruction at 0x%llx: %s (function 0x%llx)\n", (unsigned long long)a, what, (unsigned long long)c->fn);
    rt_backtrace(c);
    abort();
}
void bad_jump(CPU *c, uint64_t t, uint64_t from) {
    rt_log("[rt] jump table miss: 0x%llx -> 0x%llx\n", (unsigned long long)from, (unsigned long long)t);
    abort();
}
void missing_func(CPU *c, uint64_t t, uint64_t from) {
    gfn f = rt_find_func(t);
    if (f) { f(c); return; }
    rt_log("[rt] call to untranslated code 0x%llx from 0x%llx\n", (unsigned long long)t, (unsigned long long)from);
    rt_backtrace(c);
    abort();
}
void cpu_syscall_trap(CPU *c, uint64_t a) {
    rt_log("[rt] int/syscall at 0x%llx (function 0x%llx)\n", (unsigned long long)a, (unsigned long long)c->fn);
    abort();
}

// Unimplemented imports: log once, return 0 (lax) or abort (RT_STRICT=1).
void unimpl_import(CPU *c, const char *name) {
    static const char *seen[4096]; static int nseen;
    int dup = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == name) { dup = 1; break; }
    if (!dup && nseen < 4096) seen[nseen++] = name;
    if (!dup || rt_strict)
        rt_log("[rt] unimplemented import %s (return 0x%llx)\n", name, (unsigned long long)rd64(c->rsp));
    if (rt_strict) abort();
    c->rax = 0;
}

// Rough guest backtrace: scan the guest stack for return addresses inside .text.
void rt_backtrace(CPU *c) {
    rt_log("  in function 0x%llx; stack:", (unsigned long long)c->fn);
    int n = 0;
    for (uint64_t p = c->rsp; p < c->stack_hi && p < c->rsp + 0x2000 && n < 16; p += 8) {
        uint64_t v = rd64(p);
        if (v >= gimage_base + 0x1000 && v < gimage_base + 0x1fc3000) { rt_log(" %llx", (unsigned long long)v); n++; }
    }
    rt_log("\n");
}

// ---------------- CPU helpers ----------------
void cpu_cpuid(CPU *c) {
    uint32_t leaf = (uint32_t)c->rax, sub = (uint32_t)c->rcx; (void)sub;
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0: a = 7; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;   // "GenuineIntel"
    case 1:
        a = 0x000306A9;   // family 6 (Ivy Bridge-like)
        b = ((rt_ncpu() & 0xFF) << 16) | (8u << 8);  // logical CPU count, clflush 64
        // ecx: SSE3, PCLMUL off, SSSE3, CX16, SSE4.1, POPCNT; no SSE4.2 (pcmpistri/pcmpestri are not translated), no AVX/OSXSAVE/FMA
        cc = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 23);
        // edx: FPU, TSC, CX8, CMOV, CLFSH, MMX, FXSR, SSE, SSE2, HTT
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 19) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 28);
        break;
    case 7: a = b = cc = d = 0; break;   // no AVX2/BMI
    case 0x80000000: a = 0x80000004; break;
    case 0x80000001: cc = 1u << 0; d = (1u << 29) | (1u << 20); break;   // LAHF, LM, NX
    case 0x80000002: a = 0x6c707041; b = 0x694d2065; cc = 0x00000000; d = 0; break;   // "Apple Mi"
    default: break;
    }
    c->rax = a; c->rbx = b; c->rcx = cc; c->rdx = d;
}

double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
double sse_round(CPU *c, double v) {
    switch ((c->mxcsr >> 13) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
double rd80(uint64_t a) {
    uint64_t mant = rd64(a); uint16_t se = rd16(a + 8);
    int e = se & 0x7FFF, s = se >> 15;
    double v;
    if (e == 0 && mant == 0) v = 0;
    else if (e == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, e - 16383 - 63);
    return s ? -v : v;
}
void wr80(uint64_t a, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (v != v) { se = 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se = (uint16_t)(0x7FFF | (v < 0 ? 0x8000 : 0)); mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double f = frexp(fabs(v), &e);
        mant = (uint64_t)ldexp(f, 64); se = (uint16_t)((e - 1 + 16383) | (v < 0 ? 0x8000 : 0));
    } else if (signbit(v)) se = 0x8000;
    wr64(a, mant); wr16(a + 8, se);
}

// ---------------- guest image ----------------
// Reserve the guest image range before the host allocator can place an arena there.
static int guest_reserved;
// The guest image range is part of the executable: a zero-fill segment that the Makefile links at
// 0x140000000 (-segaddr __GUEST). dyld maps it before any allocator runs, so nothing else can land
// there. With ASLR on it slides elsewhere, and rt_reexec_fixed_layout() restarts without ASLR.
#define GUEST_SEGMENT_SIZE 0x18000000
__asm__(".zerofill __GUEST,__image,_rt_guest_segment," "0x18000000" ",14");
extern char rt_guest_segment[];
__attribute__((constructor(101))) static void reserve_guest_range(void) {
    if ((uint64_t)(uintptr_t)rt_guest_segment == gimage_base && gimage_size <= GUEST_SEGMENT_SIZE) { guest_reserved = 1; return; }
    void *want = (void *)(uintptr_t)gimage_base;
    void *p = mmap(want, gimage_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == want) { guest_reserved = 1; return; }
    if (p != MAP_FAILED) munmap(p, gimage_size);
}
// The image must sit at its preferred base: translated code uses absolute addresses.
void rt_load_image(const char *path) {
    void *want = (void *)(uintptr_t)gimage_base;
    // The range was reserved by reserve_guest_range() at startup; map over that reservation only.
    void *p = guest_reserved ? mmap(want, gimage_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
                             : mmap(want, gimage_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != want) {
        rt_log("[rt] guest image range 0x%llx is not free (got %p); mappings in the way:\n", (unsigned long long)gimage_base, p);
        mach_vm_address_t a = gimage_base; mach_vm_size_t sz; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt; mach_port_t obj;
        while (a < gimage_base + gimage_size) {
            cnt = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) break;
            if (a >= gimage_base + gimage_size) break;
            rt_log("    %llx-%llx prot %d\n", (unsigned long long)a, (unsigned long long)(a + sz), info.protection);
            a += sz;
        }
        rt_log("    RT_FIXED_LAYOUT=%s\n", getenv("RT_FIXED_LAYOUT") ? getenv("RT_FIXED_LAYOUT") : "(unset)");
        exit(1);
    }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    // headers (some code reads the PE header through __ImageBase)
    if (fread(p, 1, 0x1000, f) != 0x1000) { rt_log("short read: headers\n"); exit(1); }
    for (int i = 0; i < ngsections; i++) {
        const GSection *s = &gsections[i];
        uint32_t n = s->fsize < s->vsize ? s->fsize : s->vsize;
        fseek(f, s->fo, SEEK_SET);
        if (n && fread((void *)(uintptr_t)s->va, 1, n, f) != n) { rt_log("short read: %s\n", s->name); exit(1); }
    }
    fclose(f);
    // imports: every IAT slot points at the native implementation (or its "unimplemented" stub)
    for (int i = 0; i < ngimports; i++) wr64(gimports[i].slot, rt_host_fn(gimports[i].fn, gimports[i].name));
}

// ---------------- translated DLLs ----------------
// DLL images live in a second zero-fill segment (-segaddr __GUESTDLL 0x180000000), the usual 64-bit DLL base.
#define DLL_SEGMENT_BASE 0x180000000ull
#define DLL_SEGMENT_SIZE 0x4000000ull
__asm__(".zerofill __GUESTDLL,__images,_rt_dll_segment," "0x4000000" ",14");
extern char rt_dll_segment[];
const GModule *rt_find_module(const char *base_name) {
    for (int i = 0; i < nmodules; i++) if (!strcmp(modules[i]->name, base_name)) return modules[i];
    return NULL;
}
const GModule *rt_module_at(uint64_t a) {   // the loaded DLL containing address a (or with base a)
    for (int i = 0; i < nmodules; i++) if (module_loaded[i] && a >= modules[i]->base && a < modules[i]->base + modules[i]->size) return modules[i];
    return NULL;
}
int rt_module_loaded(const GModule *m) { for (int i = 0; i < nmodules; i++) if (modules[i] == m) return module_loaded[i]; return 0; }
// Map a DLL at its preferred base from its host file, bind its imports. 0 on failure. DllMain is the caller's job.
int rt_load_module(const GModule *m, const char *host_path) {
    int idx = -1;
    for (int i = 0; i < nmodules; i++) if (modules[i] == m) idx = i;
    if (idx < 0) return 0;
    if (module_loaded[idx]) return 1;
    void *want = PTR(m->base);
    int in_seg = (uint64_t)(uintptr_t)rt_dll_segment == DLL_SEGMENT_BASE && m->base >= DLL_SEGMENT_BASE && m->base + m->size <= DLL_SEGMENT_BASE + DLL_SEGMENT_SIZE;
    void *p = mmap(want, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | (in_seg ? MAP_FIXED : 0), -1, 0);
    if (p != want) { rt_log("[rt] cannot map %s at 0x%llx (got %p)\n", m->name, (unsigned long long)m->base, p); if (p != MAP_FAILED) munmap(p, m->size); return 0; }
    FILE *f = fopen(host_path, "rb");
    if (!f) { rt_log("[rt] %s: %s not found\n", m->name, host_path); return 0; }
    if (fread(p, 1, 0x1000, f) != 0x1000) { fclose(f); return 0; }
    for (int i = 0; i < m->nsections; i++) {
        const GSection *sc = &m->sections[i];
        uint32_t n = sc->fsize < sc->vsize ? sc->fsize : sc->vsize;
        fseek(f, sc->fo, SEEK_SET);
        if (n && fread(PTR(sc->va), 1, n, f) != n) { rt_log("[rt] %s: short read in %s\n", m->name, sc->name); fclose(f); return 0; }
    }
    fclose(f);
    for (int i = 0; i < m->nrelocs; i++) wr64(m->relocs[i], rd64(m->relocs[i]) + (uint64_t)m->delta);   // rebased image
    for (int i = 0; i < m->nimports; i++) wr64(m->imports[i].slot, rt_host_fn(m->imports[i].fn, m->imports[i].name));
    module_loaded[idx] = 1;
    rt_log("[rt] loaded %s at 0x%llx (%d functions)\n", m->name, (unsigned long long)m->base, m->nfuncs);
    return 1;
}
// An export of a translated DLL called through another module's import (the generated I_<dll>_dll_<name>
// forwarders). Like the Windows loader, the DLL is mapped and its DllMain run before the first call.
// Calling convention: the caller pushed its return address and pops it after we return, and the
// translated function pops it too at its `ret`, so the stack pointer is adjusted back afterwards.
static pthread_mutex_t mod_mu = PTHREAD_MUTEX_INITIALIZER;
void rt_module_call(CPU *c, const GModule *m, uint64_t addr) {
    if (!rt_module_loaded(m)) {
        pthread_mutex_lock(&mod_mu);
        if (!rt_module_loaded(m)) {
            char win[256]; snprintf(win, sizeof win, "%s\\%s.dll", rt_game.win_dir, m->name);
            char *host = rt_host_path(win);
            CPU save = *c;   // DllMain runs on this CPU: keep the call's argument registers
            if (host && rt_load_module(m, host) && m->entry) {
                uint64_t a[3] = { m->base, 1, 0 };
                guest_call(c, m->entry, 3, a);
            }
            *c = save;
            free(host);
        }
        pthread_mutex_unlock(&mod_mu);
    }
    gfn f = rt_find_func(addr);
    if (!f) { rt_log("[rt] %s: export 0x%llx not translated\n", m->name, (unsigned long long)addr); abort(); }
    f(c);
    c->rsp -= 8;
}

// Export lookup through the image's own PE export directory: by name, or by ordinal (name < 0x10000).
uint64_t rt_module_export(uint64_t base, uint64_t name) {
    uint64_t nt = base + rd32(base + 0x3C);
    uint32_t rva = rd32(nt + 0x88), size = rd32(nt + 0x8C);   // DataDirectory[0] of a PE32+ optional header
    if (!rva) return 0;
    uint64_t ed = base + rva;
    uint32_t ord_base = rd32(ed + 0x10), nfun = rd32(ed + 0x14), nnames = rd32(ed + 0x18);
    uint64_t funs = base + rd32(ed + 0x1C), names = base + rd32(ed + 0x20), ords = base + rd32(ed + 0x24);
    uint32_t idx = 0xFFFFFFFFu;
    if (name < 0x10000) idx = (uint32_t)name - ord_base;
    else for (uint32_t i = 0; i < nnames; i++) if (!strcmp(PTR(base + rd32(names + 4ull * i)), PTR(name))) { idx = rd16(ords + 2ull * i); break; }
    if (idx >= nfun) return 0;
    uint32_t f = rd32(funs + 4ull * idx);
    if (f >= rva && f < rva + size) return 0;   // forwarder: not supported
    return base + f;
}

// ---------------- CPUs / guest stacks ----------------
// every guest CPU, for the watchdog
#define MAXCPU 1024
static CPU *all_cpus[MAXCPU];
static int ncpus;
CPU *cpu_new(uint64_t stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    { int i = __atomic_fetch_add(&ncpus, 1, __ATOMIC_SEQ_CST); if (i < MAXCPU) all_cpus[i] = c; }
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFull;
    uint8_t *st = mmap(NULL, stack_size + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (st == MAP_FAILED) { perror("guest stack"); abort(); }
    mprotect(st, 0x10000, PROT_NONE);   // guard page
    c->stack_lo = (uint64_t)(uintptr_t)st + 0x10000;
    c->stack_hi = c->stack_lo + stack_size;
    c->rsp = c->stack_hi - 64;
    c->fcw = 0x27F;      // Windows x64 default: double precision, round to nearest
    c->mxcsr = 0x1F80;
    teb_init(c);
    return c;
}

// Call a guest function with the Win64 calling convention (first four integer args in registers).
uint64_t guest_call(CPU *c, uint64_t addr, int nargs, const uint64_t *args) {
    uint64_t save_rsp = c->rsp;
    uint64_t rsp = (c->rsp - 8 * (nargs > 4 ? nargs : 4) - 64) & ~0xFull;
    c->rsp = rsp;
    for (int i = 4; i < nargs; i++) wr64(rsp + 8 * i, args[i]);
    if (nargs > 0) c->rcx = args[0];
    if (nargs > 1) c->rdx = args[1];
    if (nargs > 2) c->r8 = args[2];
    if (nargs > 3) c->r9 = args[3];
    // floating-point args in the same positions (for callbacks taking floats)
    for (int i = 0; i < nargs && i < 4; i++) c->xmm[i].q[0] = args[i];
    PUSH(c, 0xDEADC0DE00000000ull);   // sentinel return address
    uint64_t save_fn = c->fn;
    gfn f = rt_find_func(addr);
    if (f) f(c);
    else call_ind(c, addr);
    c->fn = save_fn;
    c->rsp = save_rsp;
    return c->rax;
}

static void crash(int sig, siginfo_t *si, void *uc) {
    CPU *c = rt_cur();
    ucontext_t *u = uc;
    uint64_t pc = u->uc_mcontext->__ss.__pc, lr = u->uc_mcontext->__ss.__lr;
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_sname) rt_log("[rt] native pc in %s+0x%llx\n", di.dli_sname, (unsigned long long)(pc - (uint64_t)di.dli_saddr));
    if (dladdr((void *)lr, &di) && di.dli_sname) rt_log("[rt] native lr in %s+0x%llx\n", di.dli_sname, (unsigned long long)(lr - (uint64_t)di.dli_saddr));
    rt_log("[rt] signal %d at address %p (guest function 0x%llx)\n", sig, si->si_addr, c ? (unsigned long long)c->fn : 0ull);
    if (c) {
        rt_log("  rax=%llx rcx=%llx rdx=%llx rbx=%llx rsp=%llx rbp=%llx rsi=%llx rdi=%llx\n  r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
               (unsigned long long)c->rax, (unsigned long long)c->rcx, (unsigned long long)c->rdx, (unsigned long long)c->rbx,
               (unsigned long long)c->rsp, (unsigned long long)c->rbp, (unsigned long long)c->rsi, (unsigned long long)c->rdi,
               (unsigned long long)c->r8, (unsigned long long)c->r9, (unsigned long long)c->r10, (unsigned long long)c->r11,
               (unsigned long long)c->r12, (unsigned long long)c->r13, (unsigned long long)c->r14, (unsigned long long)c->r15);
        rt_backtrace(c);
    }
    signal(sig, SIG_DFL);
}

// every thread gets an alternate signal stack so stack overflows are still reported
void rt_thread_signals(void) {
    stack_t ss; ss.ss_size = 256 << 10; ss.ss_sp = malloc(ss.ss_size); ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}
void rt_init(void) {
    rt_strict = getenv("RT_STRICT") != NULL;
    rt_thread_signals();
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash; sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
}

// The translated code needs the guest image at 0x140000000. With ASLR the host binary can land
// there, so the process re-executes itself once (same PID) with ASLR disabled: the host binary then
// loads at its link address 0x100000000, well below the guest image.
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif
void rt_reexec_fixed_layout(char **argv) {
    const char *v = getenv("RT_FIXED_LAYOUT");
    int tries = v ? atoi(v) : 0;
    if (tries && (guest_reserved || tries >= 4)) return;
    char buf[8]; snprintf(buf, sizeof buf, "%d", tries + 1);
    setenv("RT_FIXED_LAYOUT", buf, 1);
    posix_spawnattr_t a; posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | _POSIX_SPAWN_DISABLE_ASLR);
    char path[4096]; uint32_t n = sizeof path;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (_NSGetExecutablePath(path, &n)) return;
    posix_spawn(NULL, path, NULL, &a, argv, *_NSGetEnviron());
    rt_log("[rt] re-exec without ASLR failed; continuing\n");
}

// ---------------- watchdog (RT_WATCHDOG=seconds): periodic dump of what every guest thread is doing ----------------
// Reads other threads' CPU state without stopping them (racy, for debugging only).
void h_describe_waits(uint32_t tid, char *out, size_t n);   // handles.c
static void *watchdog(void *p) {
    int secs = (int)(intptr_t)p;
    for (;;) {
        sleep(secs);
        rt_log("[watchdog] ---- %d CPUs ----\n", ncpus);
        for (int i = 0; i < ncpus && i < MAXCPU; i++) {
            CPU *c = all_cpus[i];
            if (!c || !c->gsbase) continue;
            uint32_t tid = rd32(c->gsbase + 0x48);
            char waits[256]; h_describe_waits(tid, waits, sizeof waits);
            rt_log("[watchdog] tid %u fn %llx%s stack:", tid, (unsigned long long)c->fn, waits);
            int k = 0;
            for (uint64_t q = c->rsp; q >= c->stack_lo && q < c->stack_hi && q < c->rsp + 0x1000 && k < 8; q += 8) {
                uint64_t v = rd64(q);
                if (v >= gimage_base + 0x1000 && v < gimage_base + gimage_size) { rt_log(" %llx", (unsigned long long)v); k++; }
            }
            rt_log("\n");
        }
    }
    return NULL;
}
__attribute__((constructor)) static void watchdog_start(void) {
    const char *e = getenv("RT_WATCHDOG");
    if (!e || atoi(e) <= 0) return;
    pthread_t t; pthread_create(&t, NULL, watchdog, (void *)(intptr_t)atoi(e));
}

// getenv("<env_prefix>_<name>"): the game's environment settings (game.h)
#include "game.h"
const char *rt_game_env(const char *name) {
    char k[128]; snprintf(k, sizeof k, "%s_%s", rt_game.env_prefix, name);
    return getenv(k);
}
