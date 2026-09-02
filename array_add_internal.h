#ifndef ARRAY_ADD_INTERNAL_H
#define ARRAY_ADD_INTERNAL_H

#include "array_add.h"

#if !defined(__linux__)
#error "array addition assembly requires Linux"
#endif

#if !defined(__ELF__)
#error "array addition assembly requires an ELF target"
#endif

#if !defined(__x86_64__)
#error "array addition assembly requires an x86-64 target"
#endif

/*
 * Raw assembly ABI: Linux ELF, System V AMD64, AT&T syntax.
 *
 * Array kernels receive dst, lhs, rhs, and len in rdi, rsi, rdx, and rcx.
 * Both kernels clobber those caller-saved argument registers, rax, flags, and
 * destination memory. The AVX-512F kernel additionally clobbers zmm0.
 * array_add_xgetbv0_asm clobbers the caller-saved rax, rcx, rdx, and flags.
 *
 * Every entry is a leaf: no calls, unchanged rsp, no stack use, and no unwind
 * metadata. Call the AVX-512F kernel only after the selector validates the
 * exact AMD family/model allowlist, AVX-512F, and required XCR0 state.
 */
void array_add_u32_scalar_asm(
    uint32_t *dst,
    const uint32_t *lhs,
    const uint32_t *rhs,
    size_t len
);

void array_add_u32_zen5_avx512f_asm(
    uint32_t *dst,
    const uint32_t *lhs,
    const uint32_t *rhs,
    size_t len
);

/* Call only after CPUID reports XSAVE, OSXSAVE, and AVX support. */
uint64_t array_add_xgetbv0_asm(void);

/* Pure selector: synthetic CPU data can select only a non-executable enum. */
array_add_u32_resolve_status array_add_u32_select_backend_for_cpu(
    array_add_u32_request request,
    const array_add_u32_cpu_info *info,
    array_add_u32_backend *backend_out
);

#endif
