#define _GNU_SOURCE

#include "array_add.h"
#include "array_add_stats.h"

/* Short local names keep CLI and statistics code readable. */
typedef array_add_u32_backend array_add_backend;
typedef array_add_u32_cpu_info array_add_cpu_info;
typedef array_add_u32_kernel array_add_kernel;
typedef array_add_u32_request array_add_request;
typedef array_add_u32_resolve_status array_add_status;

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

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>

#define ALIGNMENT_BYTES 64U
#define EFFECTIVE_BYTES_PER_ELEMENT 12.0
#define MINIMUM_PRACTICAL_EFFECT_FRACTION 0.05
#define PRACTICAL_SPEEDUP_THRESHOLD 1.05
#define CONFIDENCE_LEVEL 0.95
#define MAXIMUM_RELATIVE_CI_TOTAL_WIDTH 0.10
#define MIN_TIMED_INTERVAL_SECONDS 0.005
#define MIN_COMPARE_SAMPLES 101U
#define MIN_RETAINED_SAMPLES 71U
#define DEFAULT_COMPARE_SAMPLES 101U
#define DEFAULT_WARMUPS 10U
#define BOOTSTRAP_REPLICATES 10000U
#define BOOTSTRAP_SEED UINT64_C(0x6A09E667F3BCC909)

typedef enum {
    ALIGNMENT_MALLOC = 0,
    ALIGNMENT_64
} alignment_mode;

typedef enum {
    IMPL_AUTO = 0,
    IMPL_SCALAR,
    IMPL_ZEN5
} implementation_choice;

typedef struct {
    size_t elements;
    size_t samples;
    size_t warmups;
    size_t inner;
    uint64_t seed;
    uint64_t run_id;
    size_t offset_u32;
    alignment_mode alignment;
    bool run_id_seen;
} benchmark_options;

typedef struct {
    void *base;
    uint32_t *data;
    size_t bytes;
} buffer_allocation;

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

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static bool cpu_has_avx512_os_state(const array_add_cpu_info *cpu)
{
    return (cpu->xcr0 & UINT64_C(0xE6)) == UINT64_C(0xE6);
}

static bool cpu_is_zen5(const array_add_cpu_info *cpu)
{
    return cpu->genuine_amd && cpu->family == UINT32_C(0x1A);
}

static const char *status_name(array_add_status status)
{
    switch (status) {
    case ARRAY_ADD_STATUS_OK:
        return "ok";
    case ARRAY_ADD_STATUS_UNSUPPORTED:
        return "unsupported";
    case ARRAY_ADD_STATUS_INVALID:
        return "invalid";
    }
    return "unknown";
}

static const char *alignment_name(alignment_mode alignment)
{
    return alignment == ALIGNMENT_64 ? "64" : "malloc";
}

static const char *implementation_name(implementation_choice implementation)
{
    switch (implementation) {
    case IMPL_AUTO:
        return "auto";
    case IMPL_SCALAR:
        return "scalar";
    case IMPL_ZEN5:
        return "zen5";
    }
    return "invalid";
}

static array_add_request implementation_request(
    implementation_choice implementation)
{
    switch (implementation) {
    case IMPL_AUTO:
        return ARRAY_ADD_REQUEST_AUTO;
    case IMPL_SCALAR:
        return ARRAY_ADD_REQUEST_FORCE_SCALAR;
    case IMPL_ZEN5:
        return ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F;
    }
    return (array_add_request)99;
}

static void default_options(benchmark_options *options)
{
    options->elements = (size_t)1U << 24U;
    options->samples = DEFAULT_COMPARE_SAMPLES;
    options->warmups = DEFAULT_WARMUPS;
    options->inner = 1U;
    options->seed = UINT64_C(0x243F6A8885A308D3);
    options->run_id = 0U;
    options->offset_u32 = 0U;
    options->alignment = ALIGNMENT_64;
    options->run_id_seen = false;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s info\n"
            "  %s compare [--elements N --samples N --warmups N --inner N "
            "--seed N --alignment malloc|64 --offset-u32 N]\n"
            "  %s rss --impl auto|scalar|zen5 [--run-id N --elements N "
            "--seed N --alignment malloc|64 --offset-u32 N]\n",
            program, program, program);
}

static bool parse_uintmax(const char *text, uintmax_t maximum,
                          bool allow_zero, uintmax_t *out)
{
    char *end = NULL;
    uintmax_t value;

    if (text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > maximum ||
        (!allow_zero && value == 0U)) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_size_option(const char *flag, const char *text,
                              bool allow_zero, size_t *out)
{
    uintmax_t value;

    if (!parse_uintmax(text, (uintmax_t)SIZE_MAX, allow_zero, &value)) {
        fprintf(stderr, "invalid %s value: %s\n", flag, text);
        return false;
    }
    *out = (size_t)value;
    return true;
}

static bool parse_seed_option(const char *text, uint64_t *out)
{
    uintmax_t value;

    if (!parse_uintmax(text, (uintmax_t)UINT64_MAX, true, &value)) {
        fprintf(stderr, "invalid --seed value: %s\n", text);
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_run_id_option(const char *text, uint64_t *out)
{
    uintmax_t value;

    if (!parse_uintmax(text, (uintmax_t)UINT64_MAX, true, &value)) {
        fprintf(stderr, "invalid --run-id value: %s\n", text);
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool next_option_value(int argc, char **argv, int *index,
                              const char **value)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", argv[*index]);
        return false;
    }
    ++*index;
    *value = argv[*index];
    return true;
}

static bool parse_alignment(const char *text, alignment_mode *alignment)
{
    if (strcmp(text, "malloc") == 0) {
        *alignment = ALIGNMENT_MALLOC;
        return true;
    }
    if (strcmp(text, "64") == 0) {
        *alignment = ALIGNMENT_64;
        return true;
    }
    fprintf(stderr, "invalid --alignment value: %s\n", text);
    return false;
}

static bool parse_implementation(const char *text,
                                 implementation_choice *implementation)
{
    if (strcmp(text, "auto") == 0) {
        *implementation = IMPL_AUTO;
        return true;
    }
    if (strcmp(text, "scalar") == 0) {
        *implementation = IMPL_SCALAR;
        return true;
    }
    if (strcmp(text, "zen5") == 0) {
        *implementation = IMPL_ZEN5;
        return true;
    }
    fprintf(stderr, "invalid --impl value: %s\n", text);
    return false;
}

static bool parse_options(int argc, char **argv, bool compare_mode,
                          benchmark_options *options,
                          implementation_choice *implementation,
                          bool *implementation_seen)
{
    for (int index = 2; index < argc; ++index) {
        const char *flag = argv[index];
        const bool common_option =
            strcmp(flag, "--elements") == 0 ||
            strcmp(flag, "--seed") == 0 ||
            strcmp(flag, "--alignment") == 0 ||
            strcmp(flag, "--offset-u32") == 0;
        const bool compare_option =
            compare_mode &&
            (strcmp(flag, "--samples") == 0 ||
             strcmp(flag, "--warmups") == 0 ||
             strcmp(flag, "--inner") == 0);
        const bool rss_option =
            !compare_mode &&
            (strcmp(flag, "--impl") == 0 ||
             strcmp(flag, "--run-id") == 0);
        const char *value;

        if (!common_option && !compare_option && !rss_option) {
            fprintf(stderr, "unknown or unavailable option: %s\n", flag);
            return false;
        }
        if (!next_option_value(argc, argv, &index, &value)) {
            return false;
        }

        if (strcmp(flag, "--elements") == 0) {
            if (!parse_size_option(flag, value, false, &options->elements)) {
                return false;
            }
        } else if (strcmp(flag, "--seed") == 0) {
            if (!parse_seed_option(value, &options->seed)) {
                return false;
            }
        } else if (strcmp(flag, "--alignment") == 0) {
            if (!parse_alignment(value, &options->alignment)) {
                return false;
            }
        } else if (strcmp(flag, "--offset-u32") == 0) {
            if (!parse_size_option(flag, value, true,
                                   &options->offset_u32)) {
                return false;
            }
        } else if (compare_mode && strcmp(flag, "--samples") == 0) {
            if (!parse_size_option(flag, value, false, &options->samples)) {
                return false;
            }
        } else if (compare_mode && strcmp(flag, "--warmups") == 0) {
            if (!parse_size_option(flag, value, true, &options->warmups)) {
                return false;
            }
        } else if (compare_mode && strcmp(flag, "--inner") == 0) {
            if (!parse_size_option(flag, value, false, &options->inner)) {
                return false;
            }
        } else if (!compare_mode && strcmp(flag, "--impl") == 0) {
            if (!parse_implementation(value, implementation)) {
                return false;
            }
            *implementation_seen = true;
        } else if (!compare_mode && strcmp(flag, "--run-id") == 0) {
            if (!parse_run_id_option(value, &options->run_id)) {
                return false;
            }
            options->run_id_seen = true;
        }
    }
    return true;
}

static bool get_single_cpu_affinity(int *pinned_cpu, int *affinity_count)
{
    cpu_set_t affinity;
    int count;

    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        perror("sched_getaffinity");
        return false;
    }

    count = CPU_COUNT(&affinity);
    *affinity_count = count;
    if (count != 1) {
        fprintf(stderr,
                "compare requires affinity to exactly one logical CPU "
                "(found %d); run with taskset -c <cpu>\n",
                count);
        return false;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &affinity)) {
            *pinned_cpu = cpu;
            return true;
        }
    }

    fprintf(stderr, "single-CPU affinity contained no CPU\n");
    return false;
}

static bool allocate_buffer(buffer_allocation *buffer, size_t elements,
                            size_t offset_u32, alignment_mode alignment)
{
    size_t total_words;
    size_t bytes;
    void *base = NULL;

    if (!checked_add_size(elements, offset_u32, &total_words) ||
        !checked_mul_size(total_words, sizeof(uint32_t), &bytes)) {
        fprintf(stderr, "buffer size overflow\n");
        return false;
    }

    if (alignment == ALIGNMENT_64) {
        const int result = posix_memalign(&base, ALIGNMENT_BYTES, bytes);
        if (result != 0) {
            fprintf(stderr, "posix_memalign failed: %s\n", strerror(result));
            return false;
        }
    } else {
        base = malloc(bytes);
        if (base == NULL) {
            fprintf(stderr, "malloc failed for %zu bytes\n", bytes);
            return false;
        }
    }

    buffer->base = base;
    buffer->data = (uint32_t *)base + offset_u32;
    buffer->bytes = bytes;
    return true;
}

static void free_buffer(buffer_allocation *buffer)
{
    free(buffer->base);
    memset(buffer, 0, sizeof(*buffer));
}

static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t value;

    *state += UINT64_C(0x9E3779B97F4A7C15);
    value = *state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

static void initialize_buffers(buffer_allocation *lhs,
                               buffer_allocation *rhs,
                               buffer_allocation *dst, size_t elements,
                               uint64_t seed)
{
    uint64_t state = seed;

    memset(lhs->base, 0, lhs->bytes);
    memset(rhs->base, 0, rhs->bytes);
    memset(dst->base, 0, dst->bytes);

    for (size_t index = 0U; index < elements; ++index) {
        lhs->data[index] = (uint32_t)splitmix64_next(&state);
        rhs->data[index] = (uint32_t)splitmix64_next(&state);
        dst->data[index] = UINT32_C(0xA5A5A5A5);
    }

    if (elements > 0U) {
        lhs->data[0] = UINT32_MAX;
        rhs->data[0] = UINT32_C(1);
    }
    if (elements > 1U) {
        lhs->data[1] = UINT32_C(0x80000000);
        rhs->data[1] = UINT32_C(0x80000000);
    }
    if (elements > 2U) {
        lhs->data[2] = UINT32_C(1);
        rhs->data[2] = UINT32_MAX;
    }
}

static uint32_t reference_add(uint32_t lhs, uint32_t rhs)
{
    return (uint32_t)((uint64_t)lhs + (uint64_t)rhs);
}

static bool validate_output(const uint32_t *dst, const uint32_t *lhs,
                            const uint32_t *rhs, size_t elements,
                            const char *implementation, uint64_t *checksum)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);

    for (size_t index = 0U; index < elements; ++index) {
        const uint32_t expected = reference_add(lhs[index], rhs[index]);

        if (dst[index] != expected) {
            fprintf(stderr,
                    "validation failed impl=%s index=%zu expected=%08" PRIx32
                    " actual=%08" PRIx32 "\n",
                    implementation, index, expected, dst[index]);
            return false;
        }
        hash ^= (uint64_t)dst[index];
        hash *= UINT64_C(0x100000001B3);
    }

    *checksum = hash;
    return true;
}

static bool resolve_kernel(implementation_choice implementation,
                           array_add_kernel *kernel)
{
    const array_add_status status =
        array_add_resolve(implementation_request(implementation), kernel);

    if (status != ARRAY_ADD_STATUS_OK) {
        fprintf(stderr, "cannot resolve %s backend: %s\n",
                implementation_name(implementation), status_name(status));
        return false;
    }
    if (kernel->run == NULL) {
        fprintf(stderr, "resolved %s backend has no run function\n",
                implementation_name(implementation));
        return false;
    }
    return true;
}

static void print_json_backend(array_add_status status,
                               const array_add_kernel *kernel)
{
    if (status == ARRAY_ADD_STATUS_OK && kernel->run != NULL) {
        printf("\"%s\"", array_add_backend_name(kernel->backend));
    } else {
        fputs("null", stdout);
    }
}

static int run_info(void)
{
    array_add_cpu_info cpu = {0};
    array_add_kernel automatic = {0};
    array_add_kernel scalar = {0};
    array_add_kernel zen5 = {0};
    array_add_status automatic_status;
    array_add_status scalar_status;
    array_add_status zen5_status;

    array_add_detect_cpu(&cpu);
    automatic_status = array_add_resolve(ARRAY_ADD_REQUEST_AUTO, &automatic);
    scalar_status = array_add_resolve(ARRAY_ADD_REQUEST_FORCE_SCALAR, &scalar);
    zen5_status =
        array_add_resolve(ARRAY_ADD_REQUEST_FORCE_ZEN5_AVX512F, &zen5);

    printf("{\"type\":\"info\",\"cpu\":{\"genuine_amd\":%s,"
           "\"family\":%" PRIu32 ",\"model\":%" PRIu32 ","
           "\"xcr0\":\"0x%016" PRIx64 "\",\"xsave\":%s,"
           "\"osxsave\":%s,\"avx\":%s,\"avx512f\":%s,"
           "\"avx512_os_state\":%s,\"zen5\":%s},"
           "\"resolutions\":{",
           json_bool(cpu.genuine_amd), (uint32_t)cpu.family,
           (uint32_t)cpu.model, (uint64_t)cpu.xcr0, json_bool(cpu.xsave),
           json_bool(cpu.osxsave), json_bool(cpu.avx),
           json_bool(cpu.avx512f), json_bool(cpu_has_avx512_os_state(&cpu)),
           json_bool(cpu_is_zen5(&cpu)));

    printf("\"auto\":{\"status\":\"%s\",\"backend\":",
           status_name(automatic_status));
    print_json_backend(automatic_status, &automatic);
    printf("},\"scalar\":{\"status\":\"%s\",\"backend\":",
           status_name(scalar_status));
    print_json_backend(scalar_status, &scalar);
    printf("},\"zen5\":{\"status\":\"%s\",\"backend\":",
           status_name(zen5_status));
    print_json_backend(zen5_status, &zen5);
    fputs("}}}\n", stdout);
    return EXIT_SUCCESS;
}

static bool run_untimed_pair(const array_add_kernel *first,
                             const array_add_kernel *second,
                             uint32_t *dst, const uint32_t *lhs,
                             const uint32_t *rhs, size_t elements,
                             size_t inner)
{
    if (first->run == NULL || second->run == NULL) {
        return false;
    }
    for (size_t iteration = 0U; iteration < inner; ++iteration) {
        first->run(dst, lhs, rhs, elements);
    }
    for (size_t iteration = 0U; iteration < inner; ++iteration) {
        second->run(dst, lhs, rhs, elements);
    }
    return true;
}

static double timespec_difference(const struct timespec *start,
                                  const struct timespec *end)
{
    const double seconds = (double)(end->tv_sec - start->tv_sec);
    const double nanoseconds = (double)(end->tv_nsec - start->tv_nsec);

    return seconds + (nanoseconds * 1.0e-9);
}

static bool time_kernel(const array_add_kernel *kernel, uint32_t *dst,
                        const uint32_t *lhs, const uint32_t *rhs,
                        size_t elements, size_t inner, int expected_cpu,
                        double *seconds_per_call)
{
    struct timespec start;
    struct timespec end;
    int cpu_before;
    int cpu_after;
    double elapsed;

    cpu_before = sched_getcpu();
    if (cpu_before < 0) {
        perror("sched_getcpu before timed interval");
        return false;
    }
    if (cpu_before != expected_cpu) {
        fprintf(stderr,
                "CPU mismatch before timed interval: expected %d, got %d\n",
                expected_cpu, cpu_before);
        return false;
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
        perror("clock_gettime start");
        return false;
    }
    for (size_t iteration = 0U; iteration < inner; ++iteration) {
        kernel->run(dst, lhs, rhs, elements);
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) {
        perror("clock_gettime end");
        return false;
    }
    cpu_after = sched_getcpu();
    if (cpu_after < 0) {
        perror("sched_getcpu after timed interval");
        return false;
    }
    if (cpu_after != expected_cpu || cpu_after != cpu_before) {
        fprintf(stderr,
                "CPU migration or mismatch during timed interval: "
                "expected %d, before %d, after %d\n",
                expected_cpu, cpu_before, cpu_after);
        return false;
    }

    elapsed = timespec_difference(&start, &end);
    if (elapsed < MIN_TIMED_INTERVAL_SECONDS) {
        fprintf(stderr,
                "timed interval %.6f s is below %.3f s; increase --inner\n",
                elapsed, MIN_TIMED_INTERVAL_SECONDS);
        return false;
    }
    *seconds_per_call = elapsed / (double)inner;
    return true;
}

static bool allocate_pair_storage(size_t samples,
                                  array_add_pair_sample **pairs)
{
    size_t sample_bytes;

    if (!checked_mul_size(samples, sizeof(**pairs), &sample_bytes)) {
        fprintf(stderr, "sample storage size overflow\n");
        return false;
    }

    *pairs = malloc(sample_bytes);
    if (*pairs == NULL) {
        fprintf(stderr, "sample storage allocation failed\n");
        return false;
    }
    return true;
}

static const char *comparison_verdict(bool same_backend, bool adequate,
                                      double ci_low, double ci_high)
{
    if (same_backend) {
        return "same-backend-control";
    }
    if (!adequate) {
        return "inadequate-relative-ci-width";
    }
    if (ci_low >= PRACTICAL_SPEEDUP_THRESHOLD) {
        return "improved";
    }
    if (ci_low > 1.0) {
        return "positive-but-practical-not-established";
    }
    if (ci_high < 1.0) {
        return "regressed";
    }
    return "inconclusive";
}

static int run_compare(const benchmark_options *options)
{
    array_add_cpu_info cpu = {0};
    array_add_kernel scalar = {0};
    array_add_kernel candidate = {0};
    buffer_allocation lhs = {0};
    buffer_allocation rhs = {0};
    buffer_allocation dst = {0};
    array_add_pair_sample *pairs = NULL;
    array_add_pair_summary summary = {0};
    uint64_t scalar_checksum = 0U;
    uint64_t candidate_checksum = 0U;
    size_t trim_count;
    size_t retained_count;
    bool same_backend;
    bool practical_gain_proven;
    int pinned_cpu = -1;
    int affinity_count = 0;
    int result = EXIT_FAILURE;

    if (options->samples < MIN_COMPARE_SAMPLES) {
        fprintf(stderr, "compare requires at least %u measured pairs\n",
                MIN_COMPARE_SAMPLES);
        return EXIT_FAILURE;
    }
    trim_count = array_add_stats_trim_count(options->samples);
    retained_count = options->samples - (2U * trim_count);
    if (retained_count < MIN_RETAINED_SAMPLES) {
        fprintf(stderr,
                "compare requires at least %u retained pairs after trimming "
                "(got %zu)\n",
                MIN_RETAINED_SAMPLES, retained_count);
        return EXIT_FAILURE;
    }
    if (!get_single_cpu_affinity(&pinned_cpu, &affinity_count)) {
        return EXIT_FAILURE;
    }

    array_add_detect_cpu(&cpu);
    if (!resolve_kernel(IMPL_SCALAR, &scalar) ||
        !resolve_kernel(IMPL_AUTO, &candidate) ||
        !allocate_buffer(&lhs, options->elements, options->offset_u32,
                         options->alignment) ||
        !allocate_buffer(&rhs, options->elements, options->offset_u32,
                         options->alignment) ||
        !allocate_buffer(&dst, options->elements, options->offset_u32,
                         options->alignment) ||
        !allocate_pair_storage(options->samples, &pairs)) {
        goto cleanup;
    }

    initialize_buffers(&lhs, &rhs, &dst, options->elements, options->seed);

    scalar.run(dst.data, lhs.data, rhs.data, options->elements);
    if (!validate_output(dst.data, lhs.data, rhs.data, options->elements,
                         "scalar", &scalar_checksum)) {
        goto cleanup;
    }
    candidate.run(dst.data, lhs.data, rhs.data, options->elements);
    if (!validate_output(dst.data, lhs.data, rhs.data, options->elements,
                         array_add_backend_name(candidate.backend),
                         &candidate_checksum) ||
        scalar_checksum != candidate_checksum) {
        fprintf(stderr, "candidate checksum differs from scalar checksum\n");
        goto cleanup;
    }

    for (size_t warmup = 0U; warmup < options->warmups; ++warmup) {
        const bool ab_order = (warmup & 1U) == 0U;
        if (!run_untimed_pair(ab_order ? &scalar : &candidate,
                              ab_order ? &candidate : &scalar, dst.data,
                              lhs.data, rhs.data, options->elements,
                              options->inner)) {
            fprintf(stderr, "warmup failed\n");
            goto cleanup;
        }
    }

    for (size_t sample = 0U; sample < options->samples; ++sample) {
        array_add_pair_sample *pair = &pairs[sample];

        pair->sample_id = sample + 1U;
        pair->ab_order = (sample & 1U) == 0U;
        if (pair->ab_order) {
            if (!time_kernel(&scalar, dst.data, lhs.data, rhs.data,
                             options->elements, options->inner, pinned_cpu,
                             &pair->scalar_seconds) ||
                !time_kernel(&candidate, dst.data, lhs.data, rhs.data,
                             options->elements, options->inner, pinned_cpu,
                             &pair->candidate_seconds)) {
                goto cleanup;
            }
        } else if (!time_kernel(&candidate, dst.data, lhs.data, rhs.data,
                                options->elements, options->inner, pinned_cpu,
                                &pair->candidate_seconds) ||
                   !time_kernel(&scalar, dst.data, lhs.data, rhs.data,
                                options->elements, options->inner, pinned_cpu,
                                &pair->scalar_seconds)) {
            goto cleanup;
        }
    }

    if (!array_add_stats_analyze_pairs(
            pairs, options->samples, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
            CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary)) {
        fprintf(stderr, "paired statistics analysis failed\n");
        goto cleanup;
    }

    same_backend = scalar.backend == candidate.backend;
    practical_gain_proven =
        !same_backend && summary.adequacy_criterion_met &&
        summary.paired_bootstrap_ci_low >= PRACTICAL_SPEEDUP_THRESHOLD;
    printf("{\"type\":\"config\",\"command\":\"compare\","
           "\"elements\":%zu,\"samples\":%zu,\"warmups\":%zu,"
           "\"warmups_excluded_from_analysis\":true,"
           "\"inner\":%zu,\"seed\":%" PRIu64 ","
           "\"alignment\":\"%s\",\"offset_u32\":%zu,"
           "\"actual_mod_64\":{\"lhs\":%" PRIuPTR
           ",\"rhs\":%" PRIuPTR ",\"dst\":%" PRIuPTR "},"
           "\"clock\":\"CLOCK_MONOTONIC_RAW\","
           "\"pinned_cpu\":%d,\"affinity_count\":%d,"
           "\"experimental_unit\":\"paired-A/B-timing-pair\","
           "\"pairing\":\"alternating-AB-BA\","
           "\"primary_metric\":\"ln(scalar/candidate)\","
           "\"trim_fraction_each_tail\":0.15,"
           "\"trim_count_formula\":\"floor(N*15/100)\","
           "\"whole_pair_trimming\":true,"
           "\"trim_sort\":\"paired_log_ratio_ascending\","
           "\"trim_tie_policy\":"
           "\"sample_id_ascending_then_acquisition_index\","
           "\"shared_retained_pair_set_for_all_statistics\":true,"
           "\"bootstrap_method\":"
           "\"retained-only-paired-percentile\","
           "\"bootstrap_retrim_each_replicate\":false,"
           "\"confidence_level\":%.2f,"
           "\"minimum_practical_effect_fraction\":%.2f,"
           "\"practical_speedup_threshold\":%.2f,"
           "\"adequacy_max_relative_ci_total_width\":%.2f,"
           "\"adequacy_relative_ci_total_width_definition\":"
           "\"(ci_high-ci_low)/point_estimate\","
           "\"ad_hoc_outlier_removal\":false,"
           "\"baseline_backend\":\"%s\","
           "\"candidate_backend\":\"%s\","
           "\"same_backend\":%s,"
           "\"effective_bandwidth_model\":"
           "\"12-algorithmic-bytes-per-element\","
           "\"cpu\":{\"genuine_amd\":%s,\"family\":%" PRIu32
           ",\"model\":%" PRIu32 ",\"xcr0\":\"0x%016" PRIx64
           "\",\"zen5\":%s}}\n",
           options->elements, options->samples, options->warmups,
           options->inner, options->seed, alignment_name(options->alignment),
           options->offset_u32,
           (uintptr_t)lhs.data % (uintptr_t)ALIGNMENT_BYTES,
           (uintptr_t)rhs.data % (uintptr_t)ALIGNMENT_BYTES,
           (uintptr_t)dst.data % (uintptr_t)ALIGNMENT_BYTES, pinned_cpu,
           affinity_count, CONFIDENCE_LEVEL,
           MINIMUM_PRACTICAL_EFFECT_FRACTION, PRACTICAL_SPEEDUP_THRESHOLD,
           MAXIMUM_RELATIVE_CI_TOTAL_WIDTH,
           array_add_backend_name(scalar.backend),
           array_add_backend_name(candidate.backend), json_bool(same_backend),
           json_bool(cpu.genuine_amd), (uint32_t)cpu.family,
           (uint32_t)cpu.model, (uint64_t)cpu.xcr0,
           json_bool(cpu_is_zen5(&cpu)));

    for (size_t sample = 0U; sample < options->samples; ++sample) {
        const array_add_pair_sample *pair = &pairs[sample];

        printf("{\"type\":\"sample\",\"sample\":%zu,"
               "\"order\":\"%s\",\"scalar_seconds\":"
               ARRAY_ADD_JSON_DOUBLE_FORMAT ","
               "\"candidate_seconds\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
               "\"paired_speedup_scalar_over_candidate\":"
               ARRAY_ADD_JSON_DOUBLE_FORMAT ","
               "\"paired_log_ratio\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
               "\"paired_log_ratio_rank\":%zu,"
               "\"trim_class\":\"%s\",\"retained\":%s}\n",
               pair->sample_id, pair->ab_order ? "AB" : "BA",
               pair->scalar_seconds, pair->candidate_seconds,
               pair->scalar_seconds / pair->candidate_seconds,
               pair->paired_log_ratio, pair->paired_log_ratio_rank,
               array_add_stats_trim_class_name(pair->trim_class),
               json_bool(pair->trim_class == ARRAY_ADD_TRIM_RETAINED));
    }

    printf("{\"type\":\"summary\","
           "\"raw_pair_count\":%zu,\"trim_count_per_tail\":%zu,"
           "\"retained_pair_count\":%zu,"
           "\"scalar_trimmed_arithmetic_mean_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"scalar_retained_median_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"scalar_retained_mad_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_trimmed_arithmetic_mean_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_retained_median_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_retained_mad_seconds\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"ratio_of_trimmed_arithmetic_means_scalar_over_candidate\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"retained_paired_geomean_speedup_scalar_over_candidate\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_time_reduction_fraction\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"scalar_effective_algorithmic_bandwidth_gbs_12_bytes_per_element_trimmed_mean_denominator\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"scalar_effective_algorithmic_bandwidth_gbs_12_bytes_per_element_retained_median_denominator\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_effective_algorithmic_bandwidth_gbs_12_bytes_per_element_trimmed_mean_denominator\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_effective_algorithmic_bandwidth_gbs_12_bytes_per_element_retained_median_denominator\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"retained_only_paired_bootstrap_95pct_ci\":["
           ARRAY_ADD_JSON_DOUBLE_FORMAT "," ARRAY_ADD_JSON_DOUBLE_FORMAT "],"
           "\"paired_bootstrap_relative_ci_total_width\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"adequacy_max_relative_ci_total_width\":%.2f,"
           "\"adequacy_relative_ci_total_width_definition\":"
           "\"(ci_high-ci_low)/point_estimate\","
           "\"adequacy_criterion_met\":%s,"
           "\"bootstrap_replicates\":%u,"
           "\"bootstrap_seed\":%" PRIu64 ","
           "\"confidence_level\":%.2f,"
           "\"minimum_practical_effect_fraction\":%.2f,"
           "\"practical_speedup_threshold\":%.2f,"
           "\"practical_gain_proven\":%s,\"verdict\":\"%s\","
           "\"validation\":{\"elementwise\":true,"
           "\"scalar_checksum\":\"%016" PRIx64 "\","
           "\"candidate_checksum\":\"%016" PRIx64 "\"}}\n",
           summary.raw_pair_count, summary.trim_count_per_tail,
           summary.retained_pair_count,
           summary.scalar_trimmed_mean_seconds,
           summary.scalar_retained_median_seconds,
           summary.scalar_retained_mad_seconds,
           summary.candidate_trimmed_mean_seconds,
           summary.candidate_retained_median_seconds,
           summary.candidate_retained_mad_seconds,
           summary.ratio_of_trimmed_means,
           summary.paired_geomean_speedup,
           summary.candidate_time_reduction_fraction,
           ((double)options->elements * EFFECTIVE_BYTES_PER_ELEMENT) /
               summary.scalar_trimmed_mean_seconds / 1.0e9,
           ((double)options->elements * EFFECTIVE_BYTES_PER_ELEMENT) /
               summary.scalar_retained_median_seconds / 1.0e9,
           ((double)options->elements * EFFECTIVE_BYTES_PER_ELEMENT) /
               summary.candidate_trimmed_mean_seconds / 1.0e9,
           ((double)options->elements * EFFECTIVE_BYTES_PER_ELEMENT) /
               summary.candidate_retained_median_seconds / 1.0e9,
           summary.paired_bootstrap_ci_low,
           summary.paired_bootstrap_ci_high,
           summary.paired_bootstrap_relative_ci_total_width,
           MAXIMUM_RELATIVE_CI_TOTAL_WIDTH,
           json_bool(summary.adequacy_criterion_met), BOOTSTRAP_REPLICATES,
           BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
           MINIMUM_PRACTICAL_EFFECT_FRACTION, PRACTICAL_SPEEDUP_THRESHOLD,
           json_bool(practical_gain_proven),
           comparison_verdict(same_backend,
                              summary.adequacy_criterion_met,
                              summary.paired_bootstrap_ci_low,
                              summary.paired_bootstrap_ci_high),
           scalar_checksum, candidate_checksum);

    result = EXIT_SUCCESS;

cleanup:
    free(pairs);
    free_buffer(&dst);
    free_buffer(&rhs);
    free_buffer(&lhs);
    return result;
}

static int run_rss(const benchmark_options *options,
                   implementation_choice implementation)
{
    array_add_kernel kernel = {0};
    buffer_allocation lhs = {0};
    buffer_allocation rhs = {0};
    buffer_allocation dst = {0};
    struct rusage usage;
    uint64_t checksum = 0U;
    size_t data_bytes;
    int result = EXIT_FAILURE;

    if (!checked_mul_size(options->elements, sizeof(uint32_t), &data_bytes) ||
        !resolve_kernel(implementation, &kernel) ||
        !allocate_buffer(&lhs, options->elements, options->offset_u32,
                         options->alignment) ||
        !allocate_buffer(&rhs, options->elements, options->offset_u32,
                         options->alignment) ||
        !allocate_buffer(&dst, options->elements, options->offset_u32,
                         options->alignment)) {
        goto cleanup;
    }

    initialize_buffers(&lhs, &rhs, &dst, options->elements, options->seed);
    kernel.run(dst.data, lhs.data, rhs.data, options->elements);

    if (!validate_output(dst.data, lhs.data, rhs.data, options->elements,
                         array_add_backend_name(kernel.backend), &checksum)) {
        goto cleanup;
    }
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        goto cleanup;
    }

    printf("{\"type\":\"rss_result\",\"result\":\"ok\","
           "\"run_id\":%" PRIu64 ",\"run_id_supplied\":%s,"
           "\"requested_impl\":\"%s\",\"resolved_backend\":\"%s\","
           "\"elements\":%zu,\"data_bytes_per_array\":%zu,"
           "\"array_allocations\":3,\"seed\":%" PRIu64 ","
           "\"alignment\":\"%s\",\"offset_u32\":%zu,"
           "\"actual_mod_64\":{\"lhs\":%" PRIuPTR
           ",\"rhs\":%" PRIuPTR ",\"dst\":%" PRIuPTR "},"
           "\"pages_fully_touched\":true,\"backend_executions\":1,"
           "\"elementwise_valid\":true,\"checksum\":\"%016" PRIx64
           "\",\"linux_ru_maxrss_kib\":%ld,\"timing_claim\":false}\n",
           options->run_id, json_bool(options->run_id_seen),
           implementation_name(implementation),
           array_add_backend_name(kernel.backend), options->elements,
           data_bytes, options->seed, alignment_name(options->alignment),
           options->offset_u32,
           (uintptr_t)lhs.data % (uintptr_t)ALIGNMENT_BYTES,
           (uintptr_t)rhs.data % (uintptr_t)ALIGNMENT_BYTES,
           (uintptr_t)dst.data % (uintptr_t)ALIGNMENT_BYTES, checksum,
           usage.ru_maxrss);

    result = EXIT_SUCCESS;

cleanup:
    free_buffer(&dst);
    free_buffer(&rhs);
    free_buffer(&lhs);
    return result;
}

int main(int argc, char **argv)
{
    benchmark_options options;
    implementation_choice implementation = IMPL_AUTO;
    bool implementation_seen = false;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        return run_info();
    }

    default_options(&options);
    if (strcmp(argv[1], "compare") == 0) {
        if (!parse_options(argc, argv, true, &options, &implementation,
                           &implementation_seen)) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        return run_compare(&options);
    }
    if (strcmp(argv[1], "rss") == 0) {
        if (!parse_options(argc, argv, false, &options, &implementation,
                           &implementation_seen) ||
            !implementation_seen) {
            if (!implementation_seen) {
                fprintf(stderr, "rss requires --impl auto|scalar|zen5\n");
            }
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        return run_rss(&options, implementation);
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
