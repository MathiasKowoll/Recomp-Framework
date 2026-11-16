// x86-64 trampoline for the instruction fuzzer: run_snippet(State *s, void *code)
// State: 16 GPRs (rax rcx rdx rbx rsp rbp rsi rdi r8-r15; rsp ignored) at 0, xmm0-15 at 128.
    .text
    .globl _run_snippet
_run_snippet:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rdi, g_state(%rip)
    mov %rsi, g_code(%rip)
    movdqu 128(%rdi), %xmm0
    movdqu 144(%rdi), %xmm1
    movdqu 160(%rdi), %xmm2
    movdqu 176(%rdi), %xmm3
    movdqu 192(%rdi), %xmm4
    movdqu 208(%rdi), %xmm5
    movdqu 224(%rdi), %xmm6
    movdqu 240(%rdi), %xmm7
    movdqu 256(%rdi), %xmm8
    movdqu 272(%rdi), %xmm9
    movdqu 288(%rdi), %xmm10
    movdqu 304(%rdi), %xmm11
    movdqu 320(%rdi), %xmm12
    movdqu 336(%rdi), %xmm13
    movdqu 352(%rdi), %xmm14
    movdqu 368(%rdi), %xmm15
    pushq $0x206                 // flags as the translated code starts them: all clear, PF set
    popfq
    mov 0(%rdi), %rax
    mov 8(%rdi), %rcx
    mov 16(%rdi), %rdx
    mov 24(%rdi), %rbx
    mov 40(%rdi), %rbp
    mov 48(%rdi), %rsi
    mov 64(%rdi), %r8
    mov 72(%rdi), %r9
    mov 80(%rdi), %r10
    mov 88(%rdi), %r11
    mov 96(%rdi), %r12
    mov 104(%rdi), %r13
    mov 112(%rdi), %r14
    mov 120(%rdi), %r15
    mov 56(%rdi), %rdi
    call *g_code(%rip)
    push %rdi
    mov g_state(%rip), %rdi
    mov %rax, 0(%rdi)
    pop %rax
    mov %rax, 56(%rdi)
    mov %rcx, 8(%rdi)
    mov %rdx, 16(%rdi)
    mov %rbx, 24(%rdi)
    mov %rbp, 40(%rdi)
    mov %rsi, 48(%rdi)
    mov %r8, 64(%rdi)
    mov %r9, 72(%rdi)
    mov %r10, 80(%rdi)
    mov %r11, 88(%rdi)
    mov %r12, 96(%rdi)
    mov %r13, 104(%rdi)
    mov %r14, 112(%rdi)
    mov %r15, 120(%rdi)
    movdqu %xmm0, 128(%rdi)
    movdqu %xmm1, 144(%rdi)
    movdqu %xmm2, 160(%rdi)
    movdqu %xmm3, 176(%rdi)
    movdqu %xmm4, 192(%rdi)
    movdqu %xmm5, 208(%rdi)
    movdqu %xmm6, 224(%rdi)
    movdqu %xmm7, 240(%rdi)
    movdqu %xmm8, 256(%rdi)
    movdqu %xmm9, 272(%rdi)
    movdqu %xmm10, 288(%rdi)
    movdqu %xmm11, 304(%rdi)
    movdqu %xmm12, 320(%rdi)
    movdqu %xmm13, 336(%rdi)
    movdqu %xmm14, 352(%rdi)
    movdqu %xmm15, 368(%rdi)
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
    .data
g_state: .quad 0
g_code: .quad 0
