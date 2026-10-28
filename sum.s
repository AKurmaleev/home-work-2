.section .rodata
sum_format:
    .string "%ld\n"

.text
.globl main
.type main, @function
main:
    pushq %rbp
    movq %rsp, %rbp

    # %rax stores the first addend and then the sum.
    movq $20, %rax
    # %rcx stores the second addend; addq adds it to %rax.
    movq $22, %rcx
    addq %rcx, %rax

    # printf receives the format in %rdi and the sum in %rsi.
    movq %rax, %rsi
    leaq sum_format(%rip), %rdi
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
