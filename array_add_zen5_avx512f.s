.text
.globl array_add_u32_zen5_avx512f_asm
.hidden array_add_u32_zen5_avx512f_asm
.type array_add_u32_zen5_avx512f_asm, @function
array_add_u32_zen5_avx512f_asm:
    cmpq $16, %rcx
    jb .Lzen5_tail

.Lzen5_vector_loop:
    vmovdqu32 (%rsi), %zmm0
    vpaddd (%rdx), %zmm0, %zmm0
    vmovdqu32 %zmm0, (%rdi)
    addq $64, %rsi
    addq $64, %rdx
    addq $64, %rdi
    subq $16, %rcx
    cmpq $16, %rcx
    jae .Lzen5_vector_loop

.Lzen5_tail:
    testq %rcx, %rcx
    je .Lzen5_done

.Lzen5_scalar_loop:
    movl (%rsi), %eax
    addl (%rdx), %eax
    movl %eax, (%rdi)
    addq $4, %rsi
    addq $4, %rdx
    addq $4, %rdi
    decq %rcx
    jne .Lzen5_scalar_loop

.Lzen5_done:
    ret
.size array_add_u32_zen5_avx512f_asm, .-array_add_u32_zen5_avx512f_asm

.section .note.GNU-stack,"",@progbits
