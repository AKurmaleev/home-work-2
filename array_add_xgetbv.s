.text
.globl array_add_xgetbv0_asm
.hidden array_add_xgetbv0_asm
.type array_add_xgetbv0_asm, @function
array_add_xgetbv0_asm:
    xorl %ecx, %ecx
    xgetbv
    shlq $32, %rdx
    orq %rdx, %rax
    ret
.size array_add_xgetbv0_asm, .-array_add_xgetbv0_asm

.section .note.GNU-stack,"",@progbits
