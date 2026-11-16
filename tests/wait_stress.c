// Stress test of the kernel32 wait objects (runtime/handles.c): many threads release a semaphore / set an
// auto-reset event while others wait on it (blocking, timed and zero-timeout polls). Every semaphore release must
// be consumed exactly once; an auto-reset event never wakes more waiters than it was set; nothing may hang.
// Build against a game's built runtime (in the game project, after `make boot`): compile this file with
// -I$(FW)/runtime and link it like build/boot, but without build/fw/boot.o and with a stub
// `int rt_relaunch(int n, char **a) { return 0; }`. Exit status 0 = OK.
#include "cpu.h"
#include "rt.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void I_kernel32_dll_CreateSemaphoreW(CPU *c);
void I_kernel32_dll_ReleaseSemaphore(CPU *c);
void I_kernel32_dll_CreateEventW(CPU *c);
void I_kernel32_dll_SetEvent(CPU *c);
void I_kernel32_dll_WaitForSingleObject(CPU *c);
void I_kernel32_dll_WaitForMultipleObjects(CPU *c);

enum { PRODUCERS = 4, CONSUMERS = 8, PER_PRODUCER = 200000 };
static uint64_t sem, ev;
static _Atomic long released, taken, sets, ev_wakes;
static _Atomic int producers_done;

static uint32_t wait1(uint64_t h, uint32_t ms) { CPU c = {0}; c.rcx = h; c.rdx = ms; I_kernel32_dll_WaitForSingleObject(&c); return (uint32_t)c.rax; }

static void *producer(void *p) {
    for (int i = 0; i < PER_PRODUCER; i++) {
        CPU c = {0}; c.rcx = sem; c.rdx = 1; c.r8 = 0;
        I_kernel32_dll_ReleaseSemaphore(&c); released++;
        if ((i & 7) == 0) { CPU e = {0}; e.rcx = ev; I_kernel32_dll_SetEvent(&e); sets++; }
    }
    producers_done++;
    return NULL;
}
static void *consumer(void *p) {
    long k = (long)p;
    for (;;) {
        // polls, short timed waits and long blocking waits (not infinite: once the
        // producers are done, a waiter that started blocking would correctly wait forever)
        uint32_t ms = (k % 3 == 0) ? 0 : (k % 3 == 1) ? 2 : 50;
        uint32_t r = wait1(sem, ms);
        if (r == 0) taken++;
        else if (producers_done == PRODUCERS && taken == released) break;
        if ((k & 1) && wait1(ev, 0) == 0) ev_wakes++;
    }
    return NULL;
}

int main(void) {
    CPU c = {0};
    c.rdx = 0; c.r8 = 0x7FFFFFFF; I_kernel32_dll_CreateSemaphoreW(&c); sem = c.rax;
    c = (CPU){0}; c.rdx = 0; c.r8 = 0; I_kernel32_dll_CreateEventW(&c); ev = c.rax;   // auto-reset, not signaled
    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    for (long i = 0; i < CONSUMERS; i++) pthread_create(&ct[i], NULL, consumer, (void *)i);
    for (long i = 0; i < PRODUCERS; i++) pthread_create(&pt[i], NULL, producer, NULL);
    for (int i = 0; i < PRODUCERS; i++) pthread_join(pt[i], NULL);
    for (int i = 0; i < CONSUMERS; i++) pthread_join(ct[i], NULL);
    long left = 0;
    while (wait1(sem, 0) == 0) left++;
    printf("semaphore: released %ld, taken %ld, left %ld -> %s\n", (long)released, (long)taken, left,
           released == taken + left && left == 0 ? "OK" : "WRONG");
    printf("event: set %ld times, %ld wakes -> %s\n", (long)sets, (long)ev_wakes, ev_wakes <= sets ? "OK" : "WRONG");
    return !(released == taken && ev_wakes <= sets);
}
