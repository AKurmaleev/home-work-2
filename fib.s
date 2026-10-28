.equ FIB_INDEX, 10

.section .rodata
fib_format:
    .string "F(%ld) = %ld\n"

.text
.globl main
.type main, @function
main:
    pushq %rbp
    movq %rsp, %rbp

    # %rax is the current value F(i); %rdx is the next value F(i + 1).
    xorq %rax, %rax
    movq $1, %rdx
    # %rcx counts how many state updates remain; %r8 is temporary.
    movq $FIB_INDEX, %rcx

    testq %rcx, %rcx
    je .Lfib_done

.Lfib_loop:
    movq %rax, %r8
    addq %rdx, %r8
    movq %rdx, %rax
    movq %r8, %rdx
    decq %rcx
    jne .Lfib_loop

.Lfib_done:
    movq %rax, %rdx
    movq $FIB_INDEX, %rsi
    leaq fib_format(%rip), %rdi
    xorl %eax, %eax
    call printf@PLT
    testl %eax, %eax
    js .Loutput_failed

    # libc exit cannot report a delayed stdout error to our caller.
    xorl %edi, %edi
    call fflush@PLT
    testl %eax, %eax
    setne %al
    movzbl %al, %eax
.Lreturn:
    popq %rbp
    ret
.Loutput_failed:
    movl $1, %eax
    jmp .Lreturn
.size main, .-main

.section .note.GNU-stack,"",@progbits
