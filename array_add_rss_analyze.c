#include "array_add_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_LINE_CAPACITY 8192U
#define MINIMUM_SAMPLE_COUNT 31U
#define BOOTSTRAP_REPLICATES 10000U
#define BOOTSTRAP_SEED UINT64_C(0x6A09E667F3BCC909)
#define CONFIDENCE_LEVEL 0.95
#define MINIMUM_PRACTICAL_REDUCTION 0.05
#define EQUIVALENCE_MARGIN 0.01
#define MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH 0.005

typedef struct sample_group {
    array_add_unpaired_sample *samples;
    size_t count;
    size_t capacity;
} sample_group;

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static bool append_sample(sample_group *group, double value)
{
    if (group->count == group->capacity) {
        size_t new_capacity;
        array_add_unpaired_sample *resized;

        if (group->capacity == 0U) {
            new_capacity = 32U;
        } else {
            if (group->capacity > SIZE_MAX / 2U) {
                return false;
            }
            new_capacity = group->capacity * 2U;
        }
        if (new_capacity > SIZE_MAX / sizeof(*group->samples)) {
            return false;
        }
        resized = realloc(group->samples,
                          new_capacity * sizeof(*group->samples));
        if (resized == NULL) {
            return false;
        }
        group->samples = resized;
        group->capacity = new_capacity;
    }

    if (group->count == SIZE_MAX) {
        return false;
    }
    group->samples[group->count].sample_id = group->count + 1U;
    group->samples[group->count].value = value;
    group->samples[group->count].value_rank = 0U;
    group->samples[group->count].trim_class = ARRAY_ADD_TRIM_RETAINED;
    ++group->count;
    return true;
}

static bool parse_positive_uintmax(const char *text, const char **end_out,
                                   uintmax_t *value_out)
{
    char *end = NULL;
    uintmax_t value;

    if (*text < '1' || *text > '9') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || value == 0U) {
        return false;
    }
    *end_out = end;
    *value_out = value;
    return true;
}

static bool parse_rss_line(const char *line, size_t expected_run_id,
                           double *rss_kib)
{
    static const char prefix[] =
        "{\"type\":\"rss_result\",\"result\":\"ok\",\"run_id\":";
    static const char rss_field[] = ",\"linux_ru_maxrss_kib\":";
    const char *run_end;
    const char *rss_start;
    const char *rss_end;
    uintmax_t run_id;
    uintmax_t rss_value;
    const size_t length = strlen(line);

    if (length == 0U || line[length - 1U] != '}' ||
        strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    if (!parse_positive_uintmax(line + sizeof(prefix) - 1U, &run_end,
                                &run_id) ||
        *run_end != ',' || run_id != (uintmax_t)expected_run_id) {
        return false;
    }

    rss_start = strstr(run_end, rss_field);
    if (rss_start == NULL ||
        strstr(rss_start + sizeof(rss_field) - 1U, rss_field) != NULL) {
        return false;
    }
    rss_start += sizeof(rss_field) - 1U;
    if (!parse_positive_uintmax(rss_start, &rss_end, &rss_value) ||
        (*rss_end != ',' && *rss_end != '}')) {
        return false;
    }

    *rss_kib = (double)rss_value;
    return isfinite(*rss_kib) && *rss_kib > 0.0;
}

static bool read_samples(const char *path, sample_group *group)
{
    FILE *input = fopen(path, "r");
    char line[INPUT_LINE_CAPACITY];
    size_t line_number = 0U;
    bool success = false;

    if (input == NULL) {
        fprintf(stderr, "%s: cannot open input\n", path);
        return false;
    }

    while (fgets(line, sizeof(line), input) != NULL) {
        double rss_kib;
        size_t length;

        ++line_number;
        length = strlen(line);
        if (length == 0U ||
            (line[length - 1U] != '\n' && !feof(input))) {
            fprintf(stderr, "%s:%zu: line is empty, binary, or too long\n",
                    path, line_number);
            goto cleanup;
        }
        if (line[length - 1U] == '\n') {
            line[--length] = '\0';
        }
        if (length > 0U && line[length - 1U] == '\r') {
            line[--length] = '\0';
        }
        if (group->count == SIZE_MAX ||
            !parse_rss_line(line, group->count + 1U, &rss_kib)) {
            fprintf(stderr,
                    "%s:%zu: expected rss_result/result ok, run_id %zu, "
                    "and positive linux_ru_maxrss_kib\n",
                    path, line_number, group->count + 1U);
            goto cleanup;
        }
        if (!append_sample(group, rss_kib)) {
            fprintf(stderr, "%s:%zu: sample allocation failed\n", path,
                    line_number);
            goto cleanup;
        }
    }

    if (ferror(input)) {
        fprintf(stderr, "%s: input read failed\n", path);
        goto cleanup;
    }
    if (group->count < MINIMUM_SAMPLE_COUNT) {
        fprintf(stderr, "%s: requires at least %u samples; found %zu\n", path,
                MINIMUM_SAMPLE_COUNT, group->count);
        goto cleanup;
    }
    success = true;

cleanup:
    if (fclose(input) != 0 && success) {
        fprintf(stderr, "%s: input close failed\n", path);
        success = false;
    }
    return success;
}

static void print_config(size_t baseline_count, size_t candidate_count)
{
    printf("{\"type\":\"rss_analysis_config\","
           "\"metric\":\"linux_ru_maxrss_kib\","
           "\"analysis\":\"unpaired\","
           "\"baseline_raw_count\":%zu,\"candidate_raw_count\":%zu,"
           "\"trim_fraction_per_tail\":0.15,"
           "\"trim_tie_break\":\"sample_id_then_acquisition_index\","
           "\"bootstrap_method\":"
           "\"retained-only-independent-percentile\","
           "\"bootstrap_retrim_each_replicate\":false,"
           "\"bootstrap_replicates\":%u,\"bootstrap_seed\":%" PRIu64 ","
           "\"confidence_level\":%.2f,"
           "\"minimum_practical_reduction\":%.2f,"
           "\"equivalence_margin\":%.2f,"
           "\"maximum_absolute_ci_total_width\":%.3f}\n",
           baseline_count, candidate_count, BOOTSTRAP_REPLICATES,
           BOOTSTRAP_SEED, CONFIDENCE_LEVEL, MINIMUM_PRACTICAL_REDUCTION,
           EQUIVALENCE_MARGIN, MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH);
}

static void print_samples(const char *implementation,
                          const sample_group *group)
{
    for (size_t index = 0U; index < group->count; ++index) {
        const array_add_unpaired_sample *sample = &group->samples[index];

        printf("{\"type\":\"rss_analysis_sample\","
               "\"implementation\":\"%s\",\"sample_id\":%zu,"
               "\"linux_ru_maxrss_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
               "\"value_rank\":%zu,\"trim_class\":\"%s\","
               "\"retained\":%s}\n",
               implementation, sample->sample_id, sample->value,
               sample->value_rank,
               array_add_stats_trim_class_name(sample->trim_class),
               json_bool(sample->trim_class == ARRAY_ADD_TRIM_RETAINED));
    }
}

static void print_summary(const array_add_unpaired_comparison *comparison,
                          array_add_stats_verdict verdict)
{
    printf("{\"type\":\"rss_analysis_summary\","
           "\"baseline_raw_count\":%zu,"
           "\"baseline_trim_count_per_tail\":%zu,"
           "\"baseline_retained_count\":%zu,"
           "\"baseline_trimmed_mean_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"baseline_retained_median_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"baseline_retained_mad_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_raw_count\":%zu,"
           "\"candidate_trim_count_per_tail\":%zu,"
           "\"candidate_retained_count\":%zu,"
           "\"candidate_trimmed_mean_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_retained_median_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_retained_mad_kib\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"candidate_to_baseline_trimmed_mean_ratio\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"relative_reduction\":" ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"retained_only_independent_bootstrap_95pct_ci\":["
           ARRAY_ADD_JSON_DOUBLE_FORMAT "," ARRAY_ADD_JSON_DOUBLE_FORMAT "],"
           "\"bootstrap_ci_absolute_total_width\":"
           ARRAY_ADD_JSON_DOUBLE_FORMAT ","
           "\"adequacy_criterion_met\":%s,\"verdict\":\"%s\"}\n",
           comparison->baseline.raw_count,
           comparison->baseline.trim_count_per_tail,
           comparison->baseline.retained_count,
           comparison->baseline.trimmed_mean,
           comparison->baseline.retained_median,
           comparison->baseline.retained_mad,
           comparison->candidate.raw_count,
           comparison->candidate.trim_count_per_tail,
           comparison->candidate.retained_count,
           comparison->candidate.trimmed_mean,
           comparison->candidate.retained_median,
           comparison->candidate.retained_mad,
           comparison->candidate_to_baseline_trimmed_mean_ratio,
           comparison->relative_reduction, comparison->bootstrap_ci_low,
           comparison->bootstrap_ci_high,
           comparison->bootstrap_ci_absolute_total_width,
           json_bool(comparison->adequacy_criterion_met),
           array_add_stats_verdict_name(verdict));
}

int main(int argc, char **argv)
{
    sample_group baseline = {0};
    sample_group candidate = {0};
    array_add_unpaired_comparison comparison = {0};
    array_add_stats_verdict verdict;
    int exit_code = EXIT_FAILURE;

    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <baseline.jsonl> <candidate.jsonl>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (!read_samples(argv[1], &baseline) ||
        !read_samples(argv[2], &candidate)) {
        goto cleanup;
    }
    if (!array_add_stats_compare_unpaired(
            baseline.samples, baseline.count, candidate.samples,
            candidate.count, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
            CONFIDENCE_LEVEL, MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH,
            &comparison)) {
        fprintf(stderr, "unpaired RSS statistics analysis failed\n");
        goto cleanup;
    }

    verdict = array_add_stats_classify_reduction(
        &comparison, MINIMUM_PRACTICAL_REDUCTION, EQUIVALENCE_MARGIN);
    print_config(baseline.count, candidate.count);
    print_samples("baseline", &baseline);
    print_samples("candidate", &candidate);
    print_summary(&comparison, verdict);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "output write failed\n");
        goto cleanup;
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    free(candidate.samples);
    free(baseline.samples);
    return exit_code;
}
