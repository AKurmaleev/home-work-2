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

    xorl %eax, %eax
    popq %rbp
    ret
.size main, .-main

.section .note.GNU-stack,"",@progbits
