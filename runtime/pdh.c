// pdh.dll (performance counters): "\Processor(N)\% Processor Time" and "\Processor(_Total)\..." from the
// host's per-core CPU ticks (host_processor_info). Other counters read 0.
// The game must see these succeed: its cleanup path for a missing pdh.dll frees an operator-new object
// through the engine allocator, which aborts with "Memory corruption before block" (never hit on Windows).
#include "rt.h"
#include <mach/mach.h>
#include <pthread.h>
#include <strings.h>

#define MAXCORES 64
typedef struct { uint64_t busy[MAXCORES], total[MAXCORES]; double load[MAXCORES + 1]; int have, ncores; } Query;   // load[MAXCORES] = total
typedef struct { Query *q; int core; } Counter;   // core -1: _Total, -2: unsupported (reads 0)
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;

// PdhOpenQueryW(dataSource, userData, *hQuery)
void I_pdh_dll_PdhOpenQueryW(CPU *c) { wr64(A2, G(calloc(1, sizeof(Query)))); RET(0); }
void I_pdh_dll_PdhOpenQueryA(CPU *c) { I_pdh_dll_PdhOpenQueryW(c); }
void I_pdh_dll_PdhCloseQuery(CPU *c) { RET(0); }   // counters may still point at it; leaked deliberately
// PdhAddCounterW(hQuery, path, userData, *hCounter)
static void add_counter(CPU *c, int wide) {
    char *p = A1 ? (wide ? w16_to_utf8(PTR(A1)) : strdup(PTR(A1))) : strdup("");
    Counter *k = calloc(1, sizeof *k);
    k->q = PTR(A0); k->core = -2;
    const char *a = strcasestr(p, "\\Processor("), *pt = strcasestr(p, "% Processor Time");
    if (!a) a = strcasestr(p, "\\Processor Information(");
    if (a && pt) {
        const char *in = strchr(a, '(') + 1;
        if (!strncasecmp(in, "_Total", 6)) k->core = -1;
        else { const char *comma = strchr(in, ','); int n = atoi(comma && comma < strchr(in, ')') ? comma + 1 : in); k->core = n >= 0 && n < MAXCORES ? n : -2; }
    }
    if (k->core == -2) rt_log("[pdh] counter %s not supported (reads 0)\n", p);
    free(p);
    wr64(A3, G(k));
    RET(0);
}
void I_pdh_dll_PdhAddCounterW(CPU *c) { add_counter(c, 1); }
void I_pdh_dll_PdhAddCounterA(CPU *c) { add_counter(c, 0); }
void I_pdh_dll_PdhAddEnglishCounterW(CPU *c) { add_counter(c, 1); }
void I_pdh_dll_PdhAddEnglishCounterA(CPU *c) { add_counter(c, 0); }
// PdhCollectQueryData(hQuery): sample host CPU ticks; load = busy delta / total delta since the last sample
void I_pdh_dll_PdhCollectQueryData(CPU *c) {
    Query *q = PTR(A0);
    natural_t n = 0; processor_info_array_t info; mach_msg_type_number_t cnt;
    if (!q || host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS) { RET(0); return; }
    pthread_mutex_lock(&pmu);
    double sum = 0; int m = (int)n < MAXCORES ? (int)n : MAXCORES;
    for (int i = 0; i < m; i++) {
        const integer_t *t = info + CPU_STATE_MAX * i;
        uint64_t busy = (uint32_t)t[CPU_STATE_USER] + (uint32_t)t[CPU_STATE_SYSTEM] + (uint32_t)t[CPU_STATE_NICE];
        uint64_t total = busy + (uint32_t)t[CPU_STATE_IDLE];
        if (q->have && total > q->total[i]) q->load[i] = 100.0 * (double)(busy - q->busy[i]) / (double)(total - q->total[i]);
        q->busy[i] = busy; q->total[i] = total;
        sum += q->load[i];
    }
    q->load[MAXCORES] = m ? sum / m : 0;
    q->have = 1; q->ncores = m;
    pthread_mutex_unlock(&pmu);
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
    RET(0);
}
// PdhGetFormattedCounterValue(hCounter, format, *type, PDH_FMT_COUNTERVALUE*): CStatus +0, value union +8
// format: PDH_FMT_LONG 0x100, PDH_FMT_DOUBLE 0x200, PDH_FMT_LARGE 0x400
void I_pdh_dll_PdhGetFormattedCounterValue(CPU *c) {
    Counter *k = PTR(A0); uint32_t fmt = (uint32_t)A1;
    double v = 0;
    if (k && k->q && k->core != -2) {
        pthread_mutex_lock(&pmu);
        v = k->core == -1 ? k->q->load[MAXCORES] : k->core < k->q->ncores ? k->q->load[k->core] : 0;
        pthread_mutex_unlock(&pmu);
    }
    if (A2) wr32(A2, 0);
    if (A3) {
        wr32(A3, 0); wr32(A3 + 4, 0);   // PDH_CSTATUS_VALID_DATA
        if (fmt & 0x200) wrd(A3 + 8, v);
        else if (fmt & 0x400) wr64(A3 + 8, (uint64_t)(int64_t)(v + 0.5));
        else wr64(A3 + 8, (uint64_t)(uint32_t)(int32_t)(v + 0.5));
    }
    RET(0);
}
void I_pdh_dll_PdhRemoveCounter(CPU *c) { RET(0); }
