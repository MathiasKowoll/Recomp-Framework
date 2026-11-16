#pragma once
#include <stdint.h>
#include <pthread.h>

enum { HT_EVENT = 1, HT_MUTEX, HT_SEMAPHORE, HT_THREAD, HT_FILE, HT_TIMER, HT_FIND, HT_OTHER };

typedef struct HObj {
    int type, refs;
    int manual, state, suspended;
    int64_t count, max;          // semaphore
    uint32_t owner, recursion;   // mutex
    uint32_t tid, exit_code;     // thread
    pthread_t pt;
    int fd;                      // file
    uint32_t fflags;             // file: CreateFile flags (FILE_FLAG_OVERLAPPED, DELETE_ON_CLOSE, ...)
    char *name;                  // file: host path (utf-8)
    void *data;                  // type-specific
    void (*close)(struct HObj *);
    struct WNode *waiters;       // threads blocked on this object (h_wait)
    uint64_t handle;             // its handle value (set by h_new)
} HObj;

uint64_t h_new(HObj *o);
HObj *h_get(uint64_t h);
HObj *h_alloc(int type);
void h_lock(void);
void h_unlock(void);
void h_signal_all(void);      // only for suspended-thread starts (ResumeThread)
void h_wake(HObj *o);         // hmu held: wake the threads waiting on o after its state changed
uint32_t h_wait(int n, const uint64_t *hs, int all, uint32_t ms);
uint64_t thread_self_handle(void);
