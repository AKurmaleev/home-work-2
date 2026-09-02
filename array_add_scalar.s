.text
.globl array_add_u32_scalar_asm
.hidden array_add_u32_scalar_asm
.type array_add_u32_scalar_asm, @function
array_add_u32_scalar_asm:
    testq %rcx, %rcx
    je .Lscalar_done

.Lscalar_loop:
    movl (%rsi), %eax
    addl (%rdx), %eax
    movl %eax, (%rdi)
    addq $4, %rsi
    addq $4, %rdx
    addq $4, %rdi
    decq %rcx
    jne .Lscalar_loop

.Lscalar_done:
    ret
.size array_add_u32_scalar_asm, .-array_add_u32_scalar_asm

.section .note.GNU-stack,"",@progbits
