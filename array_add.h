#ifndef ARRAY_ADD_H
#define ARRAY_ADD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Adds two uint32_t arrays with wrapping modulo-2^32 arithmetic.
 *
 * The contract applies to every function returned in array_add_u32_kernel:
 * - len == 0 permits null pointers because no memory is accessed.
 * - For len > 0, each pointer addresses len naturally aligned uint32_t values.
 * - dst may equal lhs or rhs exactly.
 * - Partial overlap between dst and either input is unsupported.
 */
typedef void (*array_add_u32_fn)(
    uint32_t *dst,
    const uint32_t *lhs,
    const uint32_t *rhs,
    size_t len
);

typedef enum array_add_u32_request {
    ARRAY_ADD_U32_AUTO = 0,
    ARRAY_ADD_U32_FORCE_SCALAR = 1,
    ARRAY_ADD_U32_FORCE_ZEN5_AVX512F = 2,
} array_add_u32_request;

typedef enum array_add_u32_backend {
    ARRAY_ADD_U32_SCALAR = 0,
    ARRAY_ADD_U32_ZEN5_AVX512F = 1,
} array_add_u32_backend;

typedef enum array_add_u32_resolve_status {
    ARRAY_ADD_U32_RESOLVE_OK = 0,
    ARRAY_ADD_U32_RESOLVE_UNSUPPORTED = 1,
    ARRAY_ADD_U32_RESOLVE_INVALID = 2,
} array_add_u32_resolve_status;

typedef struct array_add_u32_cpu_info {
    uint32_t max_basic_leaf;
    uint32_t family;
    uint32_t model;
    uint64_t xcr0;
    bool genuine_amd;
    bool xsave;
    bool osxsave;
    bool avx;
    bool avx512f;
} array_add_u32_cpu_info;

typedef struct array_add_u32_kernel {
    array_add_u32_fn run;
    array_add_u32_backend backend;
} array_add_u32_kernel;

/** Detects the current CPU and OS-enabled extended register state. */
void array_add_u32_detect_cpu(array_add_u32_cpu_info *out);


/** Detects current CPU state and resolves the requested kernel. */
array_add_u32_resolve_status array_add_u32_resolve(
    array_add_u32_request request,
    array_add_u32_kernel *out
);

/** Returns a stable diagnostic name, or "unknown" for an invalid backend. */
const char *array_add_u32_backend_name(array_add_u32_backend backend);

#ifdef __cplusplus
}
#endif

#endif
