#include "array_add_internal.h"

#include <cpuid.h>

#define ARRAY_ADD_AMD_VENDOR_EBX UINT32_C(0x68747541)
#define ARRAY_ADD_AMD_VENDOR_EDX UINT32_C(0x69746e65)
#define ARRAY_ADD_AMD_VENDOR_ECX UINT32_C(0x444d4163)
#define ARRAY_ADD_ZEN5_FAMILY UINT32_C(0x1a)
#define ARRAY_ADD_ZEN5_MODEL_44 UINT32_C(0x44)
#define ARRAY_ADD_XCR0_AVX512_MASK UINT64_C(0xe6)

/*
 * Measured Zen 5 allowlist: AuthenticAMD family 0x1A, model 0x44 only.
 * Other family 0x1A models remain on the portable scalar path until measured.
 */

static uint32_t array_add_effective_family(uint32_t signature)
{
    const uint32_t base_family = (signature >> 8U) & UINT32_C(0x0f);
    const uint32_t extended_family = (signature >> 20U) & UINT32_C(0xff);

    return base_family == UINT32_C(0x0f)
        ? base_family + extended_family
        : base_family;
}

static uint32_t array_add_effective_model(uint32_t signature)
{
    const uint32_t base_family = (signature >> 8U) & UINT32_C(0x0f);
    const uint32_t base_model = (signature >> 4U) & UINT32_C(0x0f);
    const uint32_t extended_model = (signature >> 16U) & UINT32_C(0x0f);

    if (base_family == UINT32_C(0x06) || base_family == UINT32_C(0x0f)) {
        return base_model | (extended_model << 4U);
    }

    return base_model;
}

static bool array_add_cpu_supports_zen5_avx512f(
    const array_add_u32_cpu_info *info
)
{
    const bool full_xcr0_state =
        (info->xcr0 & ARRAY_ADD_XCR0_AVX512_MASK)
        == ARRAY_ADD_XCR0_AVX512_MASK;

    return info->max_basic_leaf >= UINT32_C(7)
        && info->genuine_amd
        && info->family == ARRAY_ADD_ZEN5_FAMILY
        && info->model == ARRAY_ADD_ZEN5_MODEL_44
        && info->xsave
        && info->osxsave
        && info->avx
        && info->avx512f
        && full_xcr0_state;
}

void array_add_u32_detect_cpu(array_add_u32_cpu_info *out)
{
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;

    if (out == NULL) {
        return;
    }

    *out = (array_add_u32_cpu_info){0};

    __cpuid_count(0, 0, eax, ebx, ecx, edx);
    out->max_basic_leaf = eax;
    out->genuine_amd = ebx == ARRAY_ADD_AMD_VENDOR_EBX
        && edx == ARRAY_ADD_AMD_VENDOR_EDX
        && ecx == ARRAY_ADD_AMD_VENDOR_ECX;

    if (out->max_basic_leaf < UINT32_C(1)) {
        return;
    }

    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    out->family = array_add_effective_family(eax);
    out->model = array_add_effective_model(eax);
    out->xsave = (ecx & (UINT32_C(1) << 26U)) != 0;
    out->osxsave = (ecx & (UINT32_C(1) << 27U)) != 0;
    out->avx = (ecx & (UINT32_C(1) << 28U)) != 0;

    if (out->xsave && out->osxsave && out->avx) {
        out->xcr0 = array_add_xgetbv0_asm();
    }

    if (out->max_basic_leaf < UINT32_C(7)) {
        return;
    }

    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    out->avx512f = (ebx & (UINT32_C(1) << 16U)) != 0;
}

array_add_u32_resolve_status array_add_u32_select_backend_for_cpu(
    array_add_u32_request request,
    const array_add_u32_cpu_info *info,
    array_add_u32_backend *backend_out
)
{
    if (backend_out == NULL) {
        return ARRAY_ADD_U32_RESOLVE_INVALID;
    }

    *backend_out = ARRAY_ADD_U32_SCALAR;

    if (info == NULL) {
        return ARRAY_ADD_U32_RESOLVE_INVALID;
    }

    switch (request) {
    case ARRAY_ADD_U32_AUTO:
        if (array_add_cpu_supports_zen5_avx512f(info)) {
            *backend_out = ARRAY_ADD_U32_ZEN5_AVX512F;
        }
        return ARRAY_ADD_U32_RESOLVE_OK;

    case ARRAY_ADD_U32_FORCE_SCALAR:
        return ARRAY_ADD_U32_RESOLVE_OK;

    case ARRAY_ADD_U32_FORCE_ZEN5_AVX512F:
        if (!array_add_cpu_supports_zen5_avx512f(info)) {
            return ARRAY_ADD_U32_RESOLVE_UNSUPPORTED;
        }
        *backend_out = ARRAY_ADD_U32_ZEN5_AVX512F;
        return ARRAY_ADD_U32_RESOLVE_OK;
    }

    return ARRAY_ADD_U32_RESOLVE_INVALID;
}

array_add_u32_resolve_status array_add_u32_resolve(
    array_add_u32_request request,
    array_add_u32_kernel *out
)
{
    array_add_u32_cpu_info info;
    array_add_u32_backend backend = ARRAY_ADD_U32_SCALAR;
    array_add_u32_resolve_status status;

    if (out == NULL) {
        return ARRAY_ADD_U32_RESOLVE_INVALID;
    }

    out->run = NULL;
    out->backend = ARRAY_ADD_U32_SCALAR;

    array_add_u32_detect_cpu(&info);
    status = array_add_u32_select_backend_for_cpu(request, &info, &backend);
    if (status != ARRAY_ADD_U32_RESOLVE_OK) {
        return status;
    }

    switch (backend) {
    case ARRAY_ADD_U32_SCALAR:
        out->run = array_add_u32_scalar_asm;
        break;
    case ARRAY_ADD_U32_ZEN5_AVX512F:
        out->run = array_add_u32_zen5_avx512f_asm;
        break;
    default:
        return ARRAY_ADD_U32_RESOLVE_INVALID;
    }

    out->backend = backend;
    return ARRAY_ADD_U32_RESOLVE_OK;
}

const char *array_add_u32_backend_name(array_add_u32_backend backend)
{
    switch (backend) {
    case ARRAY_ADD_U32_SCALAR:
        return "scalar";
    case ARRAY_ADD_U32_ZEN5_AVX512F:
        return "zen5-avx512f";
    }

    return "unknown";
}
