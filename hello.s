.section .rodata
message:
    .string "Hello, world!"

.text
.globl main
.type main, @function
main:
    pushq %rbp
    movq %rsp, %rbp

    leaq message(%rip), %rdi
    call puts@PLT
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
