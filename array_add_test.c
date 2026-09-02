#define _GNU_SOURCE

#include "array_add.h"
#include "array_add_internal.h"

/* Short local names keep the exhaustive harness readable. */
typedef array_add_u32_backend array_add_backend;
typedef array_add_u32_cpu_info array_add_cpu_info;
typedef array_add_u32_kernel array_add_kernel;
typedef array_add_u32_request array_add_request;
typedef array_add_u32_resolve_status array_add_status;

#define ARRAY_ADD_BACKEND_SCALAR ARRAY_ADD_U32_SCALAR
#define ARRAY_ADD_BACKEND_ZEN5_AVX512F ARRAY_ADD_U32_ZEN5_AVX512F
#define ARRAY_ADD_REQUEST_AUTO ARRAY_ADD_U32_AUTO
#define ARRAY_ADD_REQUEST_FORCE_SCALAR ARRAY_ADD_U32_FORCE_SCALAR
#define ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F \
    ARRAY_ADD_U32_FORCE_ZEN5_AVX512F
#define ARRAY_ADD_STATUS_INVALID ARRAY_ADD_U32_RESOLVE_INVALID
#define ARRAY_ADD_STATUS_OK ARRAY_ADD_U32_RESOLVE_OK
#define ARRAY_ADD_STATUS_UNSUPPORTED ARRAY_ADD_U32_RESOLVE_UNSUPPORTED
#define array_add_backend_name array_add_u32_backend_name
#define array_add_detect_cpu array_add_u32_detect_cpu
#define array_add_resolve array_add_u32_resolve
#define array_add_select_backend_for_cpu \
    array_add_u32_select_backend_for_cpu
#define array_add_scalar_asm array_add_u32_scalar_asm
#define array_add_zen5_avx512f_asm array_add_u32_zen5_avx512f_asm

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))
#define REQUIRED_XCR0_MASK UINT64_C(0xE6)
#define ALIGNMENT_BYTES 64U
#define PREFIX_WORDS 16U
#define SUFFIX_WORDS 16U

typedef void (*test_run_fn)(uint32_t *dst, const uint32_t *lhs,
                            const uint32_t *rhs, size_t len);

typedef struct {
    size_t cases;
    size_t dispatch_checks;
    size_t guard_cases;
} test_stats;

typedef struct {
    uint32_t *storage;
    uint32_t *data;
    size_t words;
    size_t payload_start;
    size_t len;
    uint32_t canary;
} canary_buffer;

typedef struct {
    void *mapping;
    size_t mapping_bytes;
    uint32_t *data;
    size_t len;
} guarded_buffer;

typedef enum {
    ALIAS_SEPARATE = 0,
    ALIAS_DST_LHS,
    ALIAS_DST_RHS,
    ALIAS_LHS_RHS,
    ALIAS_ALL_THREE,
    ALIAS_LHS_RHS_PARTIAL
} alias_mode;

typedef struct {
    const char *name;
    test_run_fn run;
} implementation;

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_mul_size(size_t lhs, size_t rhs, size_t *out)
{
    if (lhs != 0U && rhs > SIZE_MAX / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

static uint32_t lhs_value(size_t index)
{
    static const uint32_t special[] = {
        UINT32_MAX,
        UINT32_C(0x80000000),
        UINT32_C(0),
        UINT32_C(1),
        UINT32_C(0xFFFFFFFF),
        UINT32_C(0x7FFFFFFF),
        UINT32_C(0xAAAAAAAA),
        UINT32_C(0x55555555)
    };
    uint64_t mixed;

    if (index < ARRAY_LENGTH(special)) {
        return special[index];
    }
    mixed = ((uint64_t)index + UINT64_C(0x9E3779B97F4A7C15)) *
            UINT64_C(0xBF58476D1CE4E5B9);
    mixed ^= mixed >> 31U;
    return (uint32_t)(mixed ^ (mixed >> 32U));
}

static uint32_t rhs_value(size_t index)
{
    static const uint32_t special[] = {
        UINT32_C(1),
        UINT32_C(0x80000000),
        UINT32_MAX,
        UINT32_MAX,
        UINT32_C(2),
        UINT32_C(0x80000001),
        UINT32_C(0x55555556),
        UINT32_C(0xAAAAAAAA)
    };
    uint64_t mixed;

    if (index < ARRAY_LENGTH(special)) {
        return special[index];
    }
    mixed = ((uint64_t)index + UINT64_C(0xD1B54A32D192ED03)) *
            UINT64_C(0x94D049BB133111EB);
    mixed ^= mixed >> 29U;
    return (uint32_t)(mixed ^ (mixed >> 32U));
}

static uint32_t dst_value(size_t index)
{
    return UINT32_C(0xC001D00D) ^ (uint32_t)index;
}

static uint32_t reference_add(uint32_t lhs, uint32_t rhs)
{
    return (uint32_t)((uint64_t)lhs + (uint64_t)rhs);
}

static const char *alias_name(alias_mode mode)
{
    switch (mode) {
    case ALIAS_SEPARATE:
        return "separate";
    case ALIAS_DST_LHS:
        return "dst==lhs";
    case ALIAS_DST_RHS:
        return "dst==rhs";
    case ALIAS_LHS_RHS:
        return "lhs==rhs";
    case ALIAS_ALL_THREE:
        return "dst==lhs==rhs";
    case ALIAS_LHS_RHS_PARTIAL:
        return "lhs/rhs-partial";
    }
    return "invalid";
}

static bool allocate_canary_buffer(canary_buffer *buffer, size_t len,
                                   size_t offset_u32, uint32_t canary)
{
    size_t words;
    size_t bytes;
    void *storage = NULL;

    if (!checked_add_size(PREFIX_WORDS, offset_u32, &words) ||
        !checked_add_size(words, len, &words) ||
        !checked_add_size(words, SUFFIX_WORDS, &words) ||
        !checked_mul_size(words, sizeof(uint32_t), &bytes)) {
        fprintf(stderr, "allocation size overflow\n");
        return false;
    }
    if (posix_memalign(&storage, ALIGNMENT_BYTES, bytes) != 0) {
        fprintf(stderr, "posix_memalign failed for %zu bytes\n", bytes);
        return false;
    }

    buffer->storage = storage;
    buffer->data = buffer->storage + PREFIX_WORDS + offset_u32;
    buffer->words = words;
    buffer->payload_start = PREFIX_WORDS + offset_u32;
    buffer->len = len;
    buffer->canary = canary;

    for (size_t index = 0U; index < words; ++index) {
        buffer->storage[index] = canary;
    }
    return true;
}

static void free_canary_buffer(canary_buffer *buffer)
{
    free(buffer->storage);
    memset(buffer, 0, sizeof(*buffer));
}

static bool verify_canaries(const canary_buffer *buffer, const char *which,
                            const char *implementation_name, size_t len,
                            size_t offset_u32, alias_mode mode)
{
    const size_t payload_end = buffer->payload_start + buffer->len;

    for (size_t index = 0U; index < buffer->words; ++index) {
        if ((index < buffer->payload_start || index >= payload_end) &&
            buffer->storage[index] != buffer->canary) {
            fprintf(stderr,
                    "FAIL canary impl=%s len=%zu offset=%zu alias=%s "
                    "buffer=%s index=%zu expected=%08" PRIx32
                    " actual=%08" PRIx32 "\n",
                    implementation_name, len, offset_u32, alias_name(mode),
                    which, index, buffer->canary, buffer->storage[index]);
            return false;
        }
    }
    return true;
}

static bool verify_value(uint32_t actual, uint32_t expected,
                         const char *which, const char *implementation_name,
                         size_t len, size_t offset_u32, alias_mode mode,
                         size_t index)
{
    if (actual == expected) {
        return true;
    }
    fprintf(stderr,
            "FAIL value impl=%s len=%zu offset=%zu alias=%s buffer=%s "
            "index=%zu expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
            implementation_name, len, offset_u32, alias_name(mode), which,
            index, expected, actual);
    return false;
}

static bool run_canary_case(const implementation *impl, size_t len,
                            size_t offset_u32, alias_mode mode,
                            test_stats *stats)
{
    canary_buffer lhs_buffer = {0};
    canary_buffer rhs_buffer = {0};
    canary_buffer dst_buffer = {0};
    uint32_t *dst;
    const uint32_t *lhs;
    const uint32_t *rhs;
    bool ok = false;

    if (!allocate_canary_buffer(&lhs_buffer, len, offset_u32,
                                UINT32_C(0xA17EA17E)) ||
        !allocate_canary_buffer(&rhs_buffer, len, offset_u32,
                                UINT32_C(0xB28FB28F)) ||
        !allocate_canary_buffer(&dst_buffer, len, offset_u32,
                                UINT32_C(0xC390C390))) {
        goto cleanup;
    }

    for (size_t index = 0U; index < len; ++index) {
        lhs_buffer.data[index] = lhs_value(index);
        rhs_buffer.data[index] = rhs_value(index);
        dst_buffer.data[index] = dst_value(index);
    }

    lhs = lhs_buffer.data;
    rhs = rhs_buffer.data;
    dst = dst_buffer.data;
    switch (mode) {
    case ALIAS_SEPARATE:
        break;
    case ALIAS_DST_LHS:
        dst = lhs_buffer.data;
        break;
    case ALIAS_DST_RHS:
        dst = rhs_buffer.data;
        break;
    case ALIAS_LHS_RHS:
        rhs = lhs_buffer.data;
        break;
    case ALIAS_ALL_THREE:
        dst = lhs_buffer.data;
        rhs = lhs_buffer.data;
        break;
    case ALIAS_LHS_RHS_PARTIAL:
        fprintf(stderr, "FAIL internal alias mode\n");
        goto cleanup;
    }

    impl->run(dst, lhs, rhs, len);

    for (size_t index = 0U; index < len; ++index) {
        const uint32_t sum =
            mode == ALIAS_LHS_RHS || mode == ALIAS_ALL_THREE
                ? reference_add(lhs_value(index), lhs_value(index))
                : reference_add(lhs_value(index), rhs_value(index));
        uint32_t expected_lhs = lhs_value(index);
        uint32_t expected_rhs = rhs_value(index);
        uint32_t expected_dst = dst_value(index);

        if (mode == ALIAS_SEPARATE || mode == ALIAS_LHS_RHS) {
            expected_dst = sum;
        } else if (mode == ALIAS_DST_LHS || mode == ALIAS_ALL_THREE) {
            expected_lhs = sum;
        } else {
            expected_rhs = sum;
        }

        if (!verify_value(lhs_buffer.data[index], expected_lhs, "lhs",
                          impl->name, len, offset_u32, mode, index) ||
            !verify_value(rhs_buffer.data[index], expected_rhs, "rhs",
                          impl->name, len, offset_u32, mode, index) ||
            !verify_value(dst_buffer.data[index], expected_dst, "dst",
                          impl->name, len, offset_u32, mode, index)) {
            goto cleanup;
        }
    }

    if (!verify_canaries(&lhs_buffer, "lhs", impl->name, len, offset_u32,
                         mode) ||
        !verify_canaries(&rhs_buffer, "rhs", impl->name, len, offset_u32,
                         mode) ||
        !verify_canaries(&dst_buffer, "dst", impl->name, len, offset_u32,
                         mode)) {
        goto cleanup;
    }

    ++stats->cases;
    ok = true;

cleanup:
    free_canary_buffer(&dst_buffer);
    free_canary_buffer(&rhs_buffer);
    free_canary_buffer(&lhs_buffer);
    return ok;
}

static bool run_input_overlap_case(const implementation *impl, size_t len,
                                   size_t offset_u32, test_stats *stats)
{
    canary_buffer inputs = {0};
    canary_buffer dst = {0};
    size_t input_len;
    bool ok = false;

    if (len == 0U || !checked_add_size(len, 1U, &input_len)) {
        fprintf(stderr, "FAIL invalid input-overlap length\n");
        return false;
    }
    if (!allocate_canary_buffer(&inputs, input_len, offset_u32,
                                UINT32_C(0xD4A1D4A1)) ||
        !allocate_canary_buffer(&dst, len, offset_u32,
                                UINT32_C(0xE5B2E5B2))) {
        goto cleanup;
    }

    for (size_t index = 0U; index < input_len; ++index) {
        inputs.data[index] = lhs_value(index);
    }
    for (size_t index = 0U; index < len; ++index) {
        dst.data[index] = dst_value(index);
    }

    impl->run(dst.data, inputs.data, inputs.data + 1U, len);

    for (size_t index = 0U; index < input_len; ++index) {
        if (!verify_value(inputs.data[index], lhs_value(index), "inputs",
                          impl->name, len, offset_u32,
                          ALIAS_LHS_RHS_PARTIAL, index)) {
            goto cleanup;
        }
    }
    for (size_t index = 0U; index < len; ++index) {
        const uint32_t expected =
            reference_add(lhs_value(index), lhs_value(index + 1U));

        if (!verify_value(dst.data[index], expected, "dst", impl->name, len,
                          offset_u32, ALIAS_LHS_RHS_PARTIAL, index)) {
            goto cleanup;
        }
    }

    if (!verify_canaries(&inputs, "inputs", impl->name, len, offset_u32,
                         ALIAS_LHS_RHS_PARTIAL) ||
        !verify_canaries(&dst, "dst", impl->name, len, offset_u32,
                         ALIAS_LHS_RHS_PARTIAL)) {
        goto cleanup;
    }

    ++stats->cases;
    ok = true;

cleanup:
    free_canary_buffer(&dst);
    free_canary_buffer(&inputs);
    return ok;
}

static bool allocate_guarded_buffer(guarded_buffer *buffer, size_t len,
                                    size_t page_size)
{
    size_t payload_bytes;
    size_t rounded_bytes;
    size_t data_pages;
    size_t mapping_pages;
    size_t mapping_bytes;
    unsigned char *guard_page;
    void *mapping;

    if (len == 0U ||
        !checked_mul_size(len, sizeof(uint32_t), &payload_bytes) ||
        !checked_add_size(payload_bytes, page_size - 1U, &rounded_bytes)) {
        fprintf(stderr, "guard allocation size overflow\n");
        return false;
    }
    data_pages = rounded_bytes / page_size;
    if (!checked_add_size(data_pages, 1U, &mapping_pages) ||
        !checked_mul_size(mapping_pages, page_size, &mapping_bytes)) {
        fprintf(stderr, "guard mapping size overflow\n");
        return false;
    }

    mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return false;
    }
    guard_page = (unsigned char *)mapping + (data_pages * page_size);
    if (mprotect(guard_page, page_size, PROT_NONE) != 0) {
        perror("mprotect");
        (void)munmap(mapping, mapping_bytes);
        return false;
    }

    buffer->mapping = mapping;
    buffer->mapping_bytes = mapping_bytes;
    buffer->data = (uint32_t *)(void *)(guard_page - payload_bytes);
    buffer->len = len;
    return true;
}

static void free_guarded_buffer(guarded_buffer *buffer)
{
    if (buffer->mapping != NULL) {
        (void)munmap(buffer->mapping, buffer->mapping_bytes);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static bool run_guard_case(const implementation *impl, size_t len,
                           size_t page_size, test_stats *stats)
{
    guarded_buffer lhs = {0};
    guarded_buffer rhs = {0};
    guarded_buffer dst = {0};
    bool ok = false;

    if (!allocate_guarded_buffer(&lhs, len, page_size) ||
        !allocate_guarded_buffer(&rhs, len, page_size) ||
        !allocate_guarded_buffer(&dst, len, page_size)) {
        goto cleanup;
    }

    for (size_t index = 0U; index < len; ++index) {
        lhs.data[index] = lhs_value(index);
        rhs.data[index] = rhs_value(index);
        dst.data[index] = dst_value(index);
    }

    impl->run(dst.data, lhs.data, rhs.data, len);

    for (size_t index = 0U; index < len; ++index) {
        const uint32_t expected = reference_add(lhs_value(index),
                                                rhs_value(index));
        if (!verify_value(dst.data[index], expected, "guard-dst", impl->name,
                          len, 0U, ALIAS_SEPARATE, index)) {
            goto cleanup;
        }
    }

    ++stats->cases;
    ++stats->guard_cases;
    ok = true;

cleanup:
    free_guarded_buffer(&dst);
    free_guarded_buffer(&rhs);
    free_guarded_buffer(&lhs);
    return ok;
}

static bool cpu_supports_zen5_avx512f(const array_add_cpu_info *cpu)
{
    return cpu->max_basic_leaf >= UINT32_C(7) && cpu->genuine_amd &&
           cpu->family == UINT32_C(0x1A) &&
           cpu->model == UINT32_C(0x44) && cpu->xsave && cpu->osxsave &&
           cpu->avx && cpu->avx512f &&
           (cpu->xcr0 & REQUIRED_XCR0_MASK) == REQUIRED_XCR0_MASK;
}

static bool expect_backend_selection(const char *case_name,
                                     const array_add_cpu_info *cpu,
                                     array_add_request request,
                                     array_add_status expected_status,
                                     array_add_backend expected_backend,
                                     test_stats *stats)
{
    array_add_backend backend = ARRAY_ADD_BACKEND_ZEN5_AVX512F;
    const array_add_status status =
        array_add_select_backend_for_cpu(request, cpu, &backend);

    ++stats->dispatch_checks;
    if (status != expected_status || backend != expected_backend) {
        fprintf(stderr,
                "FAIL selector case=%s request=%d expected_status=%d "
                "actual_status=%d expected_backend=%d actual_backend=%d\n",
                case_name, (int)request, (int)expected_status, (int)status,
                (int)expected_backend, (int)backend);
        return false;
    }
    return true;
}

static bool test_dispatch_matrix(test_stats *stats)
{
    enum { MATRIX_CASES = 13 };
    const uint64_t xcr0_bits[] = {
        UINT64_C(0x02), UINT64_C(0x04), UINT64_C(0x20),
        UINT64_C(0x40), UINT64_C(0x80)
    };
    const char *xcr0_names[] = {
        "xcr0-missing-xmm", "xcr0-missing-ymm", "xcr0-missing-opmask",
        "xcr0-missing-zmm-hi256", "xcr0-missing-hi16-zmm"
    };
    array_add_cpu_info full = {
        .max_basic_leaf = UINT32_C(7),
        .family = UINT32_C(0x1A),
        .model = UINT32_C(0x44),
        .xcr0 = REQUIRED_XCR0_MASK,
        .genuine_amd = true,
        .xsave = true,
        .osxsave = true,
        .avx = true,
        .avx512f = true
    };
    struct {
        const char *name;
        array_add_cpu_info cpu;
        bool vector_expected;
    } cases[MATRIX_CASES];
    size_t count = 0U;

#define ADD_NEGATIVE_CASE(case_label, field_name, field_value)                 \
    do {                                                                        \
        cases[count].name = (case_label);                                       \
        cases[count].cpu = full;                                                \
        cases[count].cpu.field_name = (field_value);                            \
        cases[count].vector_expected = false;                                   \
        ++count;                                                                \
    } while (false)

    ADD_NEGATIVE_CASE("non-amd", genuine_amd, false);
    ADD_NEGATIVE_CASE("wrong-family", family, UINT32_C(0x19));
    ADD_NEGATIVE_CASE("wrong-model", model, UINT32_C(0x45));
    ADD_NEGATIVE_CASE("missing-xsave", xsave, false);
    ADD_NEGATIVE_CASE("missing-osxsave", osxsave, false);
    ADD_NEGATIVE_CASE("missing-avx", avx, false);

    for (size_t index = 0U; index < ARRAY_LENGTH(xcr0_bits); ++index) {
        cases[count].name = xcr0_names[index];
        cases[count].cpu = full;
        cases[count].cpu.xcr0 &= ~xcr0_bits[index];
        cases[count].vector_expected = false;
        ++count;
    }

    ADD_NEGATIVE_CASE("missing-avx512f", avx512f, false);
    cases[count].name = "full-zen5";
    cases[count].cpu = full;
    cases[count].vector_expected = true;
    ++count;

#undef ADD_NEGATIVE_CASE

    if (count != MATRIX_CASES) {
        fprintf(stderr, "FAIL internal dispatch matrix size\n");
        return false;
    }

    for (size_t index = 0U; index < count; ++index) {
        const array_add_backend auto_backend =
            cases[index].vector_expected ? ARRAY_ADD_BACKEND_ZEN5_AVX512F
                                         : ARRAY_ADD_BACKEND_SCALAR;
        const array_add_status vector_status =
            cases[index].vector_expected ? ARRAY_ADD_STATUS_OK
                                         : ARRAY_ADD_STATUS_UNSUPPORTED;
        const array_add_backend vector_backend =
            cases[index].vector_expected ? ARRAY_ADD_BACKEND_ZEN5_AVX512F
                                         : ARRAY_ADD_BACKEND_SCALAR;

        if (!expect_backend_selection(
                cases[index].name, &cases[index].cpu, ARRAY_ADD_REQUEST_AUTO,
                ARRAY_ADD_STATUS_OK, auto_backend, stats) ||
            !expect_backend_selection(
                cases[index].name, &cases[index].cpu,
                ARRAY_ADD_REQUEST_FORCE_SCALAR, ARRAY_ADD_STATUS_OK,
                ARRAY_ADD_BACKEND_SCALAR, stats) ||
            !expect_backend_selection(
                cases[index].name, &cases[index].cpu,
                ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F, vector_status,
                vector_backend, stats)) {
            return false;
        }
    }

    {
        const array_add_request invalid_request = (array_add_request)99;

        if (!expect_backend_selection(
                "invalid-request", &full, invalid_request,
                ARRAY_ADD_STATUS_INVALID, ARRAY_ADD_BACKEND_SCALAR, stats) ||
            !expect_backend_selection(
                "null-cpu", NULL, ARRAY_ADD_REQUEST_AUTO,
                ARRAY_ADD_STATUS_INVALID, ARRAY_ADD_BACKEND_SCALAR, stats)) {
            return false;
        }
        ++stats->dispatch_checks;
        if (array_add_select_backend_for_cpu(ARRAY_ADD_REQUEST_AUTO, &full,
                                             NULL) !=
            ARRAY_ADD_STATUS_INVALID) {
            fprintf(stderr, "FAIL null synthetic output not rejected\n");
            return false;
        }
    }

    return true;
}

static bool test_detected_dispatch(const array_add_cpu_info *cpu,
                                   test_stats *stats,
                                   array_add_kernel *auto_kernel)
{
    array_add_kernel scalar = {
        .run = array_add_zen5_avx512f_asm,
        .backend = ARRAY_ADD_BACKEND_ZEN5_AVX512F
    };
    array_add_kernel vector = {
        .run = array_add_scalar_asm,
        .backend = ARRAY_ADD_BACKEND_SCALAR
    };
    array_add_kernel invalid = {
        .run = array_add_zen5_avx512f_asm,
        .backend = ARRAY_ADD_BACKEND_ZEN5_AVX512F
    };
    const bool vector_expected = cpu_supports_zen5_avx512f(cpu);
    const array_add_status expected_vector_status =
        vector_expected ? ARRAY_ADD_STATUS_OK : ARRAY_ADD_STATUS_UNSUPPORTED;
    const array_add_backend expected_auto_backend =
        vector_expected ? ARRAY_ADD_BACKEND_ZEN5_AVX512F
                        : ARRAY_ADD_BACKEND_SCALAR;
    const test_run_fn expected_auto_run =
        vector_expected ? array_add_zen5_avx512f_asm : array_add_scalar_asm;
    array_add_status status;

    if (!expect_backend_selection(
            "detected", cpu, ARRAY_ADD_REQUEST_AUTO, ARRAY_ADD_STATUS_OK,
            expected_auto_backend, stats) ||
        !expect_backend_selection(
            "detected", cpu, ARRAY_ADD_REQUEST_FORCE_SCALAR,
            ARRAY_ADD_STATUS_OK, ARRAY_ADD_BACKEND_SCALAR, stats) ||
        !expect_backend_selection(
            "detected", cpu, ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F,
            expected_vector_status,
            vector_expected ? ARRAY_ADD_BACKEND_ZEN5_AVX512F
                            : ARRAY_ADD_BACKEND_SCALAR,
            stats)) {
        return false;
    }

    ++stats->dispatch_checks;
    status = array_add_resolve(ARRAY_ADD_REQUEST_AUTO, auto_kernel);
    if (status != ARRAY_ADD_STATUS_OK ||
        auto_kernel->backend != expected_auto_backend ||
        auto_kernel->run != expected_auto_run) {
        fprintf(stderr, "FAIL runtime AUTO resolver symbol identity\n");
        return false;
    }

    ++stats->dispatch_checks;
    status = array_add_resolve(ARRAY_ADD_REQUEST_FORCE_SCALAR, &scalar);
    if (status != ARRAY_ADD_STATUS_OK ||
        scalar.backend != ARRAY_ADD_BACKEND_SCALAR ||
        scalar.run != array_add_scalar_asm) {
        fprintf(stderr, "FAIL runtime scalar resolver symbol identity\n");
        return false;
    }

    ++stats->dispatch_checks;
    status = array_add_resolve(ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F, &vector);
    if (status != expected_vector_status ||
        (vector_expected &&
         (vector.backend != ARRAY_ADD_BACKEND_ZEN5_AVX512F ||
          vector.run != array_add_zen5_avx512f_asm)) ||
        (!vector_expected && vector.run != NULL)) {
        fprintf(stderr, "FAIL runtime vector resolver result\n");
        return false;
    }

    ++stats->dispatch_checks;
    status = array_add_resolve((array_add_request)99, &invalid);
    if (status != ARRAY_ADD_STATUS_INVALID || invalid.run != NULL) {
        fprintf(stderr, "FAIL invalid runtime resolver result\n");
        return false;
    }

    ++stats->dispatch_checks;
    if (array_add_resolve(ARRAY_ADD_REQUEST_AUTO, NULL) !=
        ARRAY_ADD_STATUS_INVALID) {
        fprintf(stderr, "FAIL null runtime output not rejected\n");
        return false;
    }

    if (array_add_backend_name(ARRAY_ADD_BACKEND_SCALAR) == NULL ||
        array_add_backend_name(ARRAY_ADD_BACKEND_ZEN5_AVX512F) == NULL ||
        strcmp(array_add_backend_name(ARRAY_ADD_BACKEND_SCALAR),
               array_add_backend_name(ARRAY_ADD_BACKEND_ZEN5_AVX512F)) == 0) {
        fprintf(stderr, "FAIL backend names missing or ambiguous\n");
        return false;
    }
    stats->dispatch_checks += 3U;

    scalar.run(NULL, NULL, NULL, 0U);
    auto_kernel->run(NULL, NULL, NULL, 0U);
    if (vector_expected) {
        vector.run(NULL, NULL, NULL, 0U);
    }
    ++stats->cases;
    return true;
}

static bool test_implementation(const implementation *impl, size_t page_size,
                                test_stats *stats)
{
    static const size_t extra_lengths[] = {
        511U, 512U, 513U, 4095U, 4096U, 4097U
    };
    static const size_t guard_lengths[] = {
        1U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U,
        255U, 256U, 257U, 511U, 512U, 513U, 4095U, 4096U, 4097U
    };
    static const size_t overlap_lengths[] = {
        1U, 15U, 16U, 17U, 31U, 32U, 33U, 255U, 256U, 257U,
        511U, 512U, 513U
    };

    impl->run(NULL, NULL, NULL, 0U);
    ++stats->cases;

    for (size_t len = 0U; len <= 257U; ++len) {
        for (size_t offset = 0U; offset <= 15U; ++offset) {
            for (alias_mode mode = ALIAS_SEPARATE; mode <= ALIAS_ALL_THREE;
                 mode = (alias_mode)((int)mode + 1)) {
                if (!run_canary_case(impl, len, offset, mode, stats)) {
                    return false;
                }
            }
        }
    }

    for (size_t length_index = 0U;
         length_index < ARRAY_LENGTH(extra_lengths); ++length_index) {
        for (size_t offset = 0U; offset <= 15U; ++offset) {
            for (alias_mode mode = ALIAS_SEPARATE; mode <= ALIAS_ALL_THREE;
                 mode = (alias_mode)((int)mode + 1)) {
                if (!run_canary_case(impl, extra_lengths[length_index], offset,
                                     mode, stats)) {
                    return false;
                }
            }
        }
    }

    for (size_t length_index = 0U;
         length_index < ARRAY_LENGTH(overlap_lengths); ++length_index) {
        for (size_t offset = 0U; offset <= 15U; ++offset) {
            if (!run_input_overlap_case(impl, overlap_lengths[length_index],
                                        offset, stats)) {
                return false;
            }
        }
    }

    for (size_t index = 0U; index < ARRAY_LENGTH(guard_lengths); ++index) {
        if (!run_guard_case(impl, guard_lengths[index], page_size, stats)) {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    test_stats stats = {0};
    array_add_cpu_info cpu = {0};
    array_add_kernel auto_kernel = {0};
    implementation implementations[2];
    size_t implementation_count = 1U;
    const long page_size_value = sysconf(_SC_PAGESIZE);
    bool require_vector = false;
    bool vector_available;
    bool vector_executed = false;

    if (argc == 2 && strcmp(argv[1], "--require-vector") == 0) {
        require_vector = true;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--require-vector]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (page_size_value <= 0 ||
        (unsigned long)page_size_value % sizeof(uint32_t) != 0U) {
        fprintf(stderr, "FAIL unsupported page size\n");
        return EXIT_FAILURE;
    }

    array_add_detect_cpu(&cpu);
    vector_available = cpu_supports_zen5_avx512f(&cpu);

    implementations[0].name = "scalar-asm";
    implementations[0].run = array_add_scalar_asm;
    if (vector_available) {
        implementations[1].name = "zen5-avx512f-asm";
        implementations[1].run = array_add_zen5_avx512f_asm;
        implementation_count = 2U;
    }

    if (!test_dispatch_matrix(&stats) ||
        !test_detected_dispatch(&cpu, &stats, &auto_kernel)) {
        return EXIT_FAILURE;
    }

    for (size_t index = 0U; index < implementation_count; ++index) {
        if (!test_implementation(&implementations[index],
                                 (size_t)page_size_value, &stats)) {
            return EXIT_FAILURE;
        }
        if (implementations[index].run == array_add_zen5_avx512f_asm) {
            vector_executed = true;
        }
    }

    if (require_vector && !vector_executed) {
        fprintf(stderr,
                "FAIL array_add_test vector_required=true "
                "vector_executed=false\n");
        return EXIT_FAILURE;
    }

    printf("PASS array_add_test cases=%zu dispatch=%zu guard=%zu "
           "implementations=%zu vector_required=%s vector_executed=%s "
           "auto=%s\n",
           stats.cases, stats.dispatch_checks, stats.guard_cases,
           implementation_count, require_vector ? "true" : "false",
           vector_executed ? "true" : "false",
           array_add_backend_name(auto_kernel.backend));
    return EXIT_SUCCESS;
}
