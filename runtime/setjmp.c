// setjmp / longjmp for translated code.
// The translator turns each `call setjmp` into rt_setjmp_site(c) (funcs.h): a host setjmp in the translated caller's
// own frame, which stays valid until that function returns, exactly the lifetime C gives a jmp_buf. rt_sj_prepare
// snapshots the guest registers (rsp still holds the pushed return address) and hands out the host jmp_buf for the
// guest buffer in rcx. rt_longjmp restores that snapshot and jumps back; the site then returns `value` in rax.
// A game's CRT shim for longjmp (msvcrt/ucrt) calls rt_longjmp(c, buf, value).
#include "rt.h"
#include <pthread.h>
#include <setjmp.h>
#include <string.h>

typedef struct { uint64_t buf; uint32_t tid; jmp_buf host; CPU regs; int used; } JmpSlot;
static JmpSlot g_jmp[256]; static pthread_mutex_t jmp_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t sj_value; static __thread JmpSlot *sj_resume;
static JmpSlot *jmp_find(uint64_t buf, uint32_t tid, int alloc) {
    JmpSlot *freeslot = NULL;
    for (int i = 0; i < 256; i++) {
        if (g_jmp[i].used && g_jmp[i].buf == buf && g_jmp[i].tid == tid) return &g_jmp[i];
        if (!g_jmp[i].used && !freeslot) freeslot = &g_jmp[i];
    }
    if (alloc && freeslot) { freeslot->used = 1; freeslot->buf = buf; freeslot->tid = tid; return freeslot; }
    return NULL;
}
jmp_buf *rt_sj_prepare(CPU *c) {
    static jmp_buf dummy;
    uint64_t buf = c->rcx;
    pthread_mutex_lock(&jmp_mu); JmpSlot *s = jmp_find(buf, rt_thread_id(), 1); pthread_mutex_unlock(&jmp_mu);
    c->rax = 0;
    if (!s) { rt_log("[rt] setjmp: out of slots\n"); return &dummy; }
    s->regs = *c;
    wr64(buf, 0); wr64(buf + 8, c->rsp + 8);   // the guest buffer looks initialized (MSVC keeps rsp in jmp_buf[1])
    return &s->host;
}
void rt_sj_resumed(CPU *c) {   // second return from the site, after longjmp
    JmpSlot *s = sj_resume; sj_resume = NULL;
    if (!s) return;
    *c = s->regs; c->rax = sj_value;
}
void rt_longjmp(CPU *c, uint64_t buf, uint32_t value) {
    pthread_mutex_lock(&jmp_mu); JmpSlot *s = jmp_find(buf, rt_thread_id(), 0); pthread_mutex_unlock(&jmp_mu);
    if (!s) { rt_log("[rt] longjmp to a buffer never set (0x%llx)\n", (unsigned long long)buf); rt_backtrace(c); abort(); }
    sj_value = value ? value : 1; sj_resume = s;
    longjmp(s->host, 1);
}
