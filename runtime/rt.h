// Runtime interface shared by the native shims.
#pragma once
#include "cpu.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>

#define HOSTFN_BASE 0x7FF000000000ull

// Win64 calling convention: integer args rcx, rdx, r8, r9, then stack (after 32 bytes of shadow space).
// On entry to a shim the return address is at rsp.
#define A0 (c->rcx)
#define A1 (c->rdx)
#define A2 (c->r8)
#define A3 (c->r9)
#define AN(n) rd64(c->rsp + 8 + 8 * (n))     // argument n >= 4
#define AF(n) (c->xmm[n].f[0])               // float argument in position n (0..3)
#define AD(n) (c->xmm[n].d[0])
#define RET(v) (c->rax = (uint64_t)(v))
#define RETF(v) (c->xmm[0].f[0] = (float)(v))
#define RETD(v) (c->xmm[0].d[0] = (double)(v))
#define PTR(a) ((void *)(uintptr_t)(a))
#define G(p) ((uint64_t)(uintptr_t)(p))

extern int rt_strict;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_init(void);
void rt_thread_signals(void);
void rt_reexec_fixed_layout(char **argv);
void rt_load_image(const char *path);
uint64_t rt_host_fn(gfn f, const char *name);
const char *rt_host_name(uint64_t a);
gfn rt_find_func(uint64_t a);
void rt_backtrace(CPU *c);
CPU *cpu_new(uint64_t stack_size);
uint64_t guest_call(CPU *c, uint64_t addr, int nargs, const uint64_t *args);

// per-thread state (teb.c)
void teb_init(CPU *c);
CPU *rt_cur(void);
void rt_set_cur(CPU *c);
uint32_t rt_thread_id(void);
void rt_longjmp(CPU *c, uint64_t buf, uint32_t value);   // setjmp.c: longjmp for translated code (rt_setjmp_site)

// UTF-16 helpers (Windows wide strings)
size_t w16len(const uint16_t *s);
char *w16_to_utf8(const uint16_t *s);          // malloc'd
uint16_t *utf8_to_w16(const char *s);          // malloc'd
void set_last_error(uint32_t e);

// host CPU count (kernel32.c): logical processors reported to the guest, and their affinity mask
uint32_t rt_ncpu(void);
uint64_t rt_cpu_mask(void);
uint32_t get_last_error(void);

// translated DLLs (rt.c)
const GModule *rt_find_module(const char *base_name);
int rt_module_loaded(const GModule *m);
const GModule *rt_module_at(uint64_t a);
int rt_load_module(const GModule *m, const char *host_path);
void rt_register_module_tls(const GModule *m, uint64_t start, uint64_t end, uint64_t index, uint32_t zero);   // teb.c
void rt_module_tls_attach(const GModule *m);
char *rt_module_file(const GModule *m, const char *win);   // rt.c: $RT_MODULE_DIR/<name>.dll, else win mapped to the host
uint64_t rt_module_export(uint64_t base, uint64_t name);
const char *rt_guest_env(const char *name);   // kernel32.c: the guest environment (Windows variables + DXVK_/MESA_ passthrough)
char *rt_host_path(const char *win);   // files.c: guest path -> malloc'd host path, or NULL
