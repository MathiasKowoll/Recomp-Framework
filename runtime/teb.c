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
#define TLS_MODULES 64    // entries of each thread's ThreadLocalStoragePointer array
static pthread_mutex_t modtls_mu = PTHREAD_MUTEX_INITIALIZER;
static void tls_add_thread(uint64_t *arr);

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
    // static TLS (.tls template): the exe is module index 0; translated DLLs get 1, 2, ... when they load
    uint64_t n = gtls_end - gtls_start;
    uint8_t *blk = calloc(1, n + 64);
    if (n) memcpy(blk, PTR(gtls_start), n);
    uint64_t *arr = calloc(TLS_MODULES, sizeof(uint64_t));
    arr[0] = G(blk);
    wr64(t + TEB_TLSPTR, G(arr));
    if (gtls_index) wr32(gtls_index, 0);
    tls_add_thread(arr);
}

// ---------------- static TLS of translated DLLs ----------------
// The Windows loader gives each module with a .tls directory an index (written to its _tls_index) and puts a
// copy of its template in ThreadLocalStoragePointer[index] of every thread, including threads that already
// exist when the DLL is loaded. MSVC's thread-safe statics depend on it: their per-thread epoch lives there.
typedef struct { const GModule *m; uint64_t start, end, index; uint32_t zero; uint32_t slot; } ModTls;
static ModTls modtls[32]; static int nmodtls;
static uint64_t **thread_arrs; static int nthread_arrs, cap_thread_arrs;
static uint32_t next_tls_slot = 1;
static uint64_t tls_block(const ModTls *m) {
    uint64_t n = m->end - m->start;
    uint8_t *b = calloc(1, n + m->zero + 64);
    if (n) memcpy(b, PTR(m->start), n);
    return G(b);
}
static void tls_add_thread(uint64_t *arr) {
    pthread_mutex_lock(&modtls_mu);
    for (int i = 0; i < nmodtls; i++) if (modtls[i].slot) arr[modtls[i].slot] = tls_block(&modtls[i]);
    if (nthread_arrs == cap_thread_arrs) { cap_thread_arrs = cap_thread_arrs ? cap_thread_arrs * 2 : 256; thread_arrs = realloc(thread_arrs, (size_t)cap_thread_arrs * sizeof *thread_arrs); }
    thread_arrs[nthread_arrs++] = arr;
    pthread_mutex_unlock(&modtls_mu);
}
// called at startup by the module's generated tables.c
void rt_register_module_tls(const GModule *m, uint64_t start, uint64_t end, uint64_t index, uint32_t zero) {
    pthread_mutex_lock(&modtls_mu);
    if (nmodtls < 32) modtls[nmodtls++] = (ModTls){ m, start, end, index, zero, 0 };
    pthread_mutex_unlock(&modtls_mu);
}
// called by the loader once the module image is mapped and relocated, before its DllMain
void rt_module_tls_attach(const GModule *m) {
    pthread_mutex_lock(&modtls_mu);
    for (int i = 0; i < nmodtls; i++) {
        ModTls *t = &modtls[i];
        if (t->m != m || t->slot) continue;
        if (next_tls_slot >= TLS_MODULES) { rt_log("[rt] %s: out of TLS module slots\n", m->name); break; }
        t->slot = next_tls_slot++;
        wr32(t->index, t->slot);
        for (int k = 0; k < nthread_arrs; k++) thread_arrs[k][t->slot] = tls_block(t);
        rt_log("[rt] %s: TLS index %u (%llu bytes, %d threads)\n", m->name, t->slot, (unsigned long long)(t->end - t->start), nthread_arrs);
    }
    pthread_mutex_unlock(&modtls_mu);
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
