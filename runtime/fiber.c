// Win32 fibers on arm64. A fiber owns a native stack (for the nested C frames of translated code)
// and a saved copy of the guest register file. SwitchToFiber swaps both.
#include "rt.h"
#include <sys/mman.h>
#include <pthread.h>

// Guest-visible fiber object: Windows code reads FiberData at offset 0 (GetFiberData is inline),
// and the TEB's NtTib.FiberData (gs:[0x20]) points at the current fiber.
typedef struct Fiber {
    uint64_t data;          // +0: lpParameter (GetFiberData)
    uint64_t pad[3];
    uint64_t host_sp;       // saved native stack pointer
    void *host_stack;       // native stack (NULL for a converted thread)
    size_t host_size;
    uint64_t start;         // guest start routine
    CPU *cpu;               // this fiber's guest register file and guest stack
    int own_cpu;            // created by CreateFiber (freed with the fiber)
} Fiber;
// A fiber can be suspended on one thread and resumed on another, and the translated code's C frames on
// its native stack keep using the CPU they started with. So each fiber owns its CPU; switching hands the
// thread-specific parts (TEB pointer, runtime thread data) to the fiber that is about to run.

#define TEB_FIBERDATA 0x20

// swap_ctx(&save_sp, new_sp): saves callee-saved registers on the current native stack, stores sp,
// switches to new_sp and restores the registers saved there.
__attribute__((naked)) static void swap_ctx(uint64_t *save_sp, uint64_t new_sp) {
    __asm__ volatile(
        "sub sp, sp, #176\n"
        "stp x19, x20, [sp, #0]\n"  "stp x21, x22, [sp, #16]\n" "stp x23, x24, [sp, #32]\n"
        "stp x25, x26, [sp, #48]\n" "stp x27, x28, [sp, #64]\n" "stp x29, x30, [sp, #80]\n"
        "stp d8, d9, [sp, #96]\n"   "stp d10, d11, [sp, #112]\n" "stp d12, d13, [sp, #128]\n" "stp d14, d15, [sp, #144]\n"
        "mov x9, sp\n" "str x9, [x0]\n"
        "mov sp, x1\n"
        "ldp x19, x20, [sp, #0]\n"  "ldp x21, x22, [sp, #16]\n" "ldp x23, x24, [sp, #32]\n"
        "ldp x25, x26, [sp, #48]\n" "ldp x27, x28, [sp, #64]\n" "ldp x29, x30, [sp, #80]\n"
        "ldp d8, d9, [sp, #96]\n"   "ldp d10, d11, [sp, #112]\n" "ldp d12, d13, [sp, #128]\n" "ldp d14, d15, [sp, #144]\n"
        "add sp, sp, #176\n"
        "ret\n");
}

static __thread Fiber *cur_fiber;

static void fiber_entry(void);

void I_kernel32_dll_ConvertThreadToFiber(CPU *c) {
    Fiber *f = calloc(1, sizeof *f);
    f->data = A0; f->cpu = c;
    cur_fiber = f;
    wr64(c->gsbase + TEB_FIBERDATA, G(f));
    RET(G(f));
}
void I_kernel32_dll_ConvertThreadToFiberEx(CPU *c) { I_kernel32_dll_ConvertThreadToFiber(c); }
void I_kernel32_dll_ConvertFiberToThread(CPU *c) { cur_fiber = NULL; wr64(c->gsbase + TEB_FIBERDATA, 0); RET(1); }

// CreateFiberEx(commitSize, reserveSize, flags, start, param)
static uint64_t create_fiber(uint64_t stack, uint64_t start, uint64_t param) {
    Fiber *f = calloc(1, sizeof *f);
    f->data = param; f->start = start;
    // guest stack (the game's own frames) and native stack (translated code's C frames)
    if (stack < (256u << 10)) stack = 256u << 10;
    f->cpu = cpu_new(stack); f->own_cpu = 1;
    f->host_size = 2u << 20;
    f->host_stack = mmap(NULL, f->host_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    // initial native frame: swap_ctx "returns" into fiber_entry
    uint64_t sp = (G(f->host_stack) + f->host_size - 176) & ~0xFull;
    memset(PTR(sp), 0, 176);
    wr64(sp + 88, G(fiber_entry));   // x30 (return address)
    wr64(sp + 80, 0);                // x29
    f->host_sp = sp;
    return G(f);
}
void I_kernel32_dll_CreateFiberEx(CPU *c) { RET(create_fiber(A1 ? A1 : A0, A3, AN(4))); }
void I_kernel32_dll_CreateFiber(CPU *c) { RET(create_fiber(A0, A1, A2)); }

static void fiber_entry(void) {
    CPU *c = rt_cur();
    Fiber *f = cur_fiber;
    uint64_t a[1] = { f->data };
    guest_call(c, f->start, 1, a);
    rt_log("[win32] fiber start routine returned; exiting thread\n");
    pthread_exit(NULL);
}

void I_kernel32_dll_SwitchToFiber(CPU *c) {
    Fiber *to = PTR(A0), *from = cur_fiber;
    if (!from) { rt_log("[win32] SwitchToFiber from a non-fiber thread\n"); abort(); }
    if (to == from) return;
    to->cpu->gsbase = c->gsbase; to->cpu->thread = c->thread;   // the target runs on this thread now
    wr64(c->gsbase + 0x08, to->cpu->stack_hi); wr64(c->gsbase + 0x10, to->cpu->stack_lo);
    wr64(c->gsbase + TEB_FIBERDATA, G(to));
    cur_fiber = to;
    rt_set_cur(to->cpu);
    swap_ctx(&from->host_sp, to->host_sp);
    // resumed, possibly on another thread: whoever switched back gave our CPU that thread's TEB
}
void I_kernel32_dll_DeleteFiber(CPU *c) {
    Fiber *f = PTR(A0);
    if (f == cur_fiber) { pthread_exit(NULL); }
    if (f->host_stack) munmap(f->host_stack, f->host_size);
    if (f->own_cpu) {
        munmap(PTR(f->cpu->stack_lo - 0x10000), f->cpu->stack_hi - f->cpu->stack_lo + 0x10000);
        free(f->cpu);
    }
    free(f);
}
void I_kernel32_dll_IsThreadAFiber(CPU *c) { RET(cur_fiber != NULL); }
