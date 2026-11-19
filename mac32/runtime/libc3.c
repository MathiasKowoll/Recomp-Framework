// More of the C library and of pthreads: what a game linked against the 10.7-10.9 SDKs imports beyond libc2.c.
// Each is the host's own function on guest memory, or the smallest honest answer.
#include "rt.h"
#include "tables.h"
#include "game32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <wchar.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <limits.h>

#define S(n) ((char *)G2H(n))
#define RET(v) (c->eax = (uint32_t)(v))
#define RET64(v) do { uint64_t _v = (uint64_t)(v); c->eax = (uint32_t)_v; c->edx = (uint32_t)(_v >> 32); } while (0)
#define RETF(v) FPUSH((double)(v))
#define ARG64(i) ((uint64_t)ARG(i) | ((uint64_t)ARG((i) + 1) << 32))
uint32_t g_errno_addr(void);
static void seterr(void) { wr32(g_errno_addr(), (uint32_t)errno); }

// ---------- math (a float argument is one 4-byte slot, a double two; both return on the x87 stack) ----------
#define D1(name) void I__##name(CPU *c) { RETF(name(ARGD(0))); }
#define D2(name) void I__##name(CPU *c) { RETF(name(ARGD(0), ARGD(2))); }
#define F2(name) void I__##name(CPU *c) { RETF(name(ARGF(0), ARGF(1))); }
D1(acos) D1(asin) D1(atan) D1(cosh) D1(sinh) D1(tanh) D1(exp2) D1(log10) D1(rint) D1(round)
D2(fmax) D2(fmin)
F2(atan2f) F2(fmaxf) F2(fminf) F2(nextafterf)
void I__rintf(CPU *c)  { RETF(rintf(ARGF(0))); }
void I__roundf(CPU *c) { RETF(roundf(ARGF(0))); }
void I__ldexpf(CPU *c) { RETF(ldexpf(ARGF(0), (int)ARG(1))); }
void I__frexpf(CPU *c) { int e; float r = frexpf(ARGF(0), &e); wr32(ARG(1), (uint32_t)e); RETF(r); }

// ---------- what the compiler calls for itself ----------
// double or float to unsigned 64 bits; out of range is the largest, as libgcc's does
void I____fixunsdfdi(CPU *c) { double v = ARGD(0); RET64(v >= 18446744073709551616.0 ? UINT64_MAX : v > 0 ? (uint64_t)v : 0); }
void I____fixunssfdi(CPU *c) { float v = ARGF(0);  RET64(v >= 18446744073709551616.0f ? UINT64_MAX : v > 0 ? (uint64_t)v : 0); }
void I____isfinited(CPU *c) { RET(isfinite(ARGD(0)) ? 1 : 0); }
void I____isinfd(CPU *c)    { RET(isinf(ARGD(0)) ? 1 : 0); }
void I____isnand(CPU *c)    { RET(isnan(ARGD(0)) ? 1 : 0); }
void I____isinff(CPU *c)    { RET(isinf(ARGF(0)) ? 1 : 0); }
void I____isnanf(CPU *c)    { RET(isnan(ARGF(0)) ? 1 : 0); }
void I____signbitf(CPU *c)  { RET(signbit(ARGF(0)) ? 1 : 0); }
void I____bzero(CPU *c)     { MC(ARG(0), ARG(1)); memset(S(ARG(0)), 0, ARG(1)); }
void I__memset_pattern16(CPU *c) { MC(ARG(0), ARG(2)); memset_pattern16(S(ARG(0)), S(ARG(1)), ARG(2)); }

// ---------- strings ----------
void I__strlcpy(CPU *c) { MC(ARG(0), ARG(2)); RET(strlcpy(S(ARG(0)), S(ARG(1)), ARG(2))); }
void I__strlcat(CPU *c) { MC(ARG(0), ARG(2)); RET(strlcat(S(ARG(0)), S(ARG(1)), ARG(2))); }
void I__strnlen(CPU *c) { RET(strnlen(S(ARG(0)), ARG(1))); }
void I__strspn(CPU *c)  { RET(strspn(S(ARG(0)), S(ARG(1)))); }
void I__strcoll(CPU *c) { RET(strcoll(S(ARG(0)), S(ARG(1)))); }
void I__strnstr(CPU *c) { char *r = strnstr(S(ARG(0)), S(ARG(1)), ARG(2)); RET(r ? H2G(r) : 0); }
void I__strtoll(CPU *c) { char *e; long long v = strtoll(S(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) wr32(ARG(1), H2G(e)); RET64(v); }
void I__strtof_UNIX2003(CPU *c) { char *e; float v = strtof(S(ARG(0)), &e); if (ARG(1)) wr32(ARG(1), H2G(e)); RETF(v); }
void I__strsignal(CPU *c) { static uint32_t g; if (g) gfree(g); g = gstrdup(strsignal((int)ARG(0))); RET(g); }
// wchar_t is four bytes on both sides
#define W(n) ((wchar_t *)G2H(n))
void I__wcslen(CPU *c)  { RET(wcslen(W(ARG(0)))); }
void I__wcscmp(CPU *c)  { RET(wcscmp(W(ARG(0)), W(ARG(1)))); }
void I__wcsncmp(CPU *c) { RET(wcsncmp(W(ARG(0)), W(ARG(1)), ARG(2))); }
void I__wcstol(CPU *c)  { wchar_t *e; long v = wcstol(W(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) wr32(ARG(1), H2G(e)); RET(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v); }
void I__wcstoul(CPU *c) { wchar_t *e; unsigned long v = wcstoul(W(ARG(0)), &e, (int)ARG(2)); if (ARG(1)) wr32(ARG(1), H2G(e)); RET(v > UINT32_MAX ? UINT32_MAX : v); }
void I__wcstod(CPU *c)  { wchar_t *e; double v = wcstod(W(ARG(0)), &e); if (ARG(1)) wr32(ARG(1), H2G(e)); RETF(v); }
void I__mbstowcs(CPU *c) { RET(mbstowcs(ARG(0) ? W(ARG(0)) : NULL, S(ARG(1)), ARG(2))); }
void I__btowc(CPU *c)   { RET(btowc((int)ARG(0))); }
void I__wctob(CPU *c)   { RET(wctob((wint_t)ARG(0))); }

// ---------- process and files ----------
void I__getppid(CPU *c) { RET(getppid()); }
void I__isatty(CPU *c)  { RET(isatty((int)ARG(0))); }
void I__sleep(CPU *c)   { RET(sleep(ARG(0))); }
void I__umask(CPU *c)   { RET(umask((mode_t)ARG(0))); }
void I__dup(CPU *c)     { int r = dup((int)ARG(0)); if (r < 0) seterr(); RET(r); }
void I__dup2(CPU *c)    { int r = dup2((int)ARG(0), (int)ARG(1)); if (r < 0) seterr(); RET(r); }
void I__fsync(CPU *c)   { RET(fsync((int)ARG(0))); }
void I__rmdir(CPU *c)   { int r = rmdir(S(ARG(0))); if (r) seterr(); RET(r); }
void I__readlink(CPU *c) { ssize_t r = readlink(S(ARG(0)), S(ARG(1)), ARG(2)); if (r < 0) seterr(); RET(r); }
void I__gethostname(CPU *c) { RET(gethostname(S(ARG(0)), ARG(1))); }
void I__getprogname(CPU *c) { static uint32_t g; if (!g) g = gstrdup(rt32_game.exe_name ? rt32_game.exe_name : "game"); RET(g); }
// The path of the program as the game knows it: the original executable in the user's copy, which is where it
// looks for its bundle. (buf, *size) -> 0, or -1 with the size needed.
void I___NSGetExecutablePath(CPU *c) {
    char p[2048];
    snprintf(p, sizeof p, "%s/../MacOS/%s", host_resources_dir(), rt32_game.exe_name ? rt32_game.exe_name : "game");
    char real[PATH_MAX]; if (realpath(p, real)) snprintf(p, sizeof p, "%s", real);
    uint32_t need = (uint32_t)strlen(p) + 1, have = rd32(ARG(1));
    wr32(ARG(1), need);
    if (have < need) { RET(-1); return; }
    memcpy(S(ARG(0)), p, need); RET(0);
}
// uname: five fields of 256 bytes. The machine is what the program was built for.
void I__uname(CPU *c) {
    struct utsname u; uname(&u);
    char *g = S(ARG(0)); memset(g, 0, 5 * 256);
    snprintf(g, 256, "%s", u.sysname); snprintf(g + 256, 256, "%s", u.nodename); snprintf(g + 512, 256, "%s", u.release);
    snprintf(g + 768, 256, "%s", u.version); snprintf(g + 1024, 256, "i386");
    RET(0);
}

// ---------- pthreads ----------
// pthread_once_t is {long sig; char opaque[4]}. The second word is ours: 1 once the routine has run. One lock for
// all of them, recursive, because a routine may reach another once.
static pthread_mutex_t once_lock;
static pthread_once_t once_lock_init = PTHREAD_ONCE_INIT;
static void once_lock_make(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&once_lock, &a);
}
void I__pthread_once(CPU *c) {
    uint32_t once = ARG(0), fn = ARG(1);
    pthread_once(&once_lock_init, once_lock_make);
    pthread_mutex_lock(&once_lock);
    if (rd32(once + 4) != 1) { wr32(once + 4, 1); guest_call(c, fn, 0, NULL); }
    pthread_mutex_unlock(&once_lock);
    RET(0);
}
// Thread-specific data: a guest key is an index into the host's keys, and the values are guest words. A guest
// destructor is not run when a thread ends.
#define NKEYS 512
static pthread_key_t hkeys[NKEYS];
static int nkeys = 1;   // key 0 is never handed out: programs test a key against zero
void I__pthread_key_create(CPU *c) {
    int k = __atomic_fetch_add(&nkeys, 1, __ATOMIC_SEQ_CST);
    if (k >= NKEYS || pthread_key_create(&hkeys[k], NULL)) { RET(EAGAIN); return; }
    wr32(ARG(0), (uint32_t)k); RET(0);
}
void I__pthread_key_delete(CPU *c) { RET(0); }
void I__pthread_setspecific(CPU *c) {
    uint32_t k = ARG(0);
    RET(k && k < NKEYS ? pthread_setspecific(hkeys[k], (void *)(uintptr_t)ARG(1)) : EINVAL);
}
void I__pthread_getspecific(CPU *c) { uint32_t k = ARG(0); RET(k && k < NKEYS ? (uint32_t)(uintptr_t)pthread_getspecific(hkeys[k]) : 0); }
void I__pthread_equal(CPU *c) { RET(ARG(0) == ARG(1)); }
void I__pthread_main_np(CPU *c) { RET(pthread_main_np()); }   // the guest's main thread is the host's
void I__pthread_setname_np(CPU *c) { pthread_setname_np(S(ARG(0))); RET(0); }
void I__pthread_getname_np(CPU *c) { if (ARG(2)) S(ARG(1))[0] = 0; RET(0); }
void I__pthread_mutexattr_destroy_UNIX2003(CPU *c) { RET(0); }
// Scheduling: threads keep the host's default, and a program that asks is told so.
void I__sched_get_priority_max(CPU *c) { RET(sched_get_priority_max((int)ARG(0))); }
void I__sched_get_priority_min(CPU *c) { RET(sched_get_priority_min((int)ARG(0))); }
void I__pthread_getschedparam(CPU *c) { if (ARG(1)) wr32(ARG(1), SCHED_OTHER); if (ARG(2)) wr32(ARG(2), 31); RET(0); }
void I__pthread_setschedparam(CPU *c) { RET(0); }

// ---------- libdispatch: the one call that needs no queue ----------
// A block is {isa, flags, reserved, invoke, ...} and invoke takes the block itself.
void I__dispatch_once(CPU *c) {
    uint32_t pred = ARG(0), block = ARG(1);
    pthread_once(&once_lock_init, once_lock_make);
    pthread_mutex_lock(&once_lock);
    if (rd32(pred) == 0) { uint32_t a[1] = { block }; guest_call(c, rd32(block + 12), 1, a); wr32(pred, 0xFFFFFFFFu); }
    pthread_mutex_unlock(&once_lock);
}
