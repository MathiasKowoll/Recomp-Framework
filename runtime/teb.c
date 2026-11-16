// Windows thread/process environment blocks for guest threads (reached through gs:).
#include "rt.h"
#include <pthread.h>
#include <sys/mman.h>

// TEB offsets (x64)
#define TEB_STACK_BASE  0x08
#define TEB_STACK_LIMIT 0x10
#define TEB_SELF        0x30
#define TEB_PID         0x40
#define TEB_TID         0x48
#define TEB_TLSPTR      0x58     // ThreadLocalStoragePointer: per-module static TLS blocks
#define TEB_PEB         0x60
#define TEB_LASTERROR   0x68
#define TEB_TLSSLOTS    0x1480   // TlsAlloc slots 0..63
#define TEB_SIZE        0x2000

static uint8_t *peb;
static __thread CPU *cur;
static uint32_t next_tid = 0x100;

CPU *rt_cur(void) { return cur; }
void rt_set_cur(CPU *c) { cur = c; }
uint32_t rt_thread_id(void) { return cur ? rd32(cur->gsbase + TEB_TID) : 0; }

void set_last_error(uint32_t e) { if (cur) wr32(cur->gsbase + TEB_LASTERROR, e); }
uint32_t get_last_error(void) { return cur ? rd32(cur->gsbase + TEB_LASTERROR) : 0; }

uint64_t g_process_heap;   // set by the kernel32 layer

void teb_init(CPU *c) {
    if (!peb) {
        peb = calloc(1, 0x1000);
        wr64(G(peb) + 0x10, gimage_base);   // ImageBaseAddress
    }
    wr64(G(peb) + 0x30, g_process_heap);
    uint8_t *teb = calloc(1, TEB_SIZE);
    uint64_t t = G(teb);
    c->gsbase = t;
    wr64(t + TEB_SELF, t);
    wr64(t + TEB_STACK_BASE, c->stack_hi);
    wr64(t + TEB_STACK_LIMIT, c->stack_lo);
    wr64(t + TEB_PEB, G(peb));
    wr64(t + TEB_PID, 0x1000);
    wr64(t + TEB_TID, __atomic_fetch_add(&next_tid, 4, __ATOMIC_SEQ_CST));
    // static TLS (.tls template), module index 0
    uint64_t n = gtls_end - gtls_start;
    uint8_t *blk = calloc(1, n + 64);
    if (n) memcpy(blk, PTR(gtls_start), n);
    uint64_t *arr = calloc(4, sizeof(uint64_t));
    arr[0] = G(blk);
    wr64(t + TEB_TLSPTR, G(arr));
    if (gtls_index) wr32(gtls_index, 0);
}

// TlsAlloc / TlsGetValue / TlsSetValue: slots live in each TEB
static uint64_t tls_used;   // bitmap of 64 slots
static pthread_mutex_t tls_mu = PTHREAD_MUTEX_INITIALIZER;
void I_kernel32_dll_TlsAlloc(CPU *c) {
    pthread_mutex_lock(&tls_mu);
    for (int i = 0; i < 64; i++) if (!(tls_used >> i & 1)) { tls_used |= 1ull << i; pthread_mutex_unlock(&tls_mu); RET(i); return; }
    pthread_mutex_unlock(&tls_mu);
    RET(0xFFFFFFFFu);
}
void I_kernel32_dll_TlsFree(CPU *c) { uint32_t i = (uint32_t)A0; if (i < 64) tls_used &= ~(1ull << i); RET(1); }
void I_kernel32_dll_TlsGetValue(CPU *c) {
    uint32_t i = (uint32_t)A0;
    set_last_error(0);
    RET(i < 64 ? rd64(c->gsbase + TEB_TLSSLOTS + 8 * i) : 0);
}
void I_kernel32_dll_TlsSetValue(CPU *c) {
    uint32_t i = (uint32_t)A0;
    if (i < 64) { wr64(c->gsbase + TEB_TLSSLOTS + 8 * i, A1); RET(1); } else RET(0);
}
void I_kernel32_dll_GetLastError(CPU *c) { RET(get_last_error()); }
void I_kernel32_dll_SetLastError(CPU *c) { set_last_error((uint32_t)A0); }
void I_kernel32_dll_GetCurrentThreadId(CPU *c) { RET(rd32(c->gsbase + TEB_TID)); }
void I_kernel32_dll_GetCurrentProcessId(CPU *c) { RET(0x1000); }
void I_kernel32_dll_GetCurrentProcess(CPU *c) { RET(~0ull); }
void I_kernel32_dll_GetCurrentThread(CPU *c) { RET(~1ull); }
