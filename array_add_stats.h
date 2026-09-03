#ifndef ARRAY_ADD_STATS_H
#define ARRAY_ADD_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_ADD_JSON_DOUBLE_FORMAT "%.17g"

typedef enum array_add_trim_class {
    ARRAY_ADD_TRIM_LOW = 0,
    ARRAY_ADD_TRIM_RETAINED,
    ARRAY_ADD_TRIM_HIGH
} array_add_trim_class;

typedef struct array_add_pair_sample {
    size_t sample_id;
    bool ab_order;
    double scalar_seconds;
    double candidate_seconds;
    double paired_log_ratio;
    size_t paired_log_ratio_rank;
    array_add_trim_class trim_class;
} array_add_pair_sample;

typedef struct array_add_pair_summary {
    size_t raw_pair_count;
    size_t trim_count_per_tail;
    size_t retained_pair_count;
    double scalar_trimmed_mean_seconds;
    double scalar_retained_median_seconds;
    double scalar_retained_mad_seconds;
    double candidate_trimmed_mean_seconds;
    double candidate_retained_median_seconds;
    double candidate_retained_mad_seconds;
    double ratio_of_trimmed_means;
    double paired_geomean_speedup;
    double candidate_time_reduction_fraction;
    double paired_bootstrap_ci_low;
    double paired_bootstrap_ci_high;
    double paired_bootstrap_relative_ci_total_width;
    bool adequacy_criterion_met;
} array_add_pair_summary;

typedef struct array_add_unpaired_sample {
    size_t sample_id;
    double value;
    size_t value_rank;
    array_add_trim_class trim_class;
} array_add_unpaired_sample;

typedef struct array_add_unpaired_summary {
    size_t raw_count;
    size_t trim_count_per_tail;
    size_t retained_count;
    double trimmed_mean;
    double retained_median;
    double retained_mad;
} array_add_unpaired_summary;

typedef struct array_add_unpaired_comparison {
    array_add_unpaired_summary baseline;
    array_add_unpaired_summary candidate;
    double candidate_to_baseline_trimmed_mean_ratio;
    double relative_reduction;
    double bootstrap_ci_low;
    double bootstrap_ci_high;
    double bootstrap_ci_absolute_total_width;
    bool adequacy_criterion_met;
} array_add_unpaired_comparison;

typedef enum array_add_stats_verdict {
    ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE = 0,
    ARRAY_ADD_STATS_VERDICT_IMPROVED,
    ARRAY_ADD_STATS_VERDICT_REGRESSED,
    ARRAY_ADD_STATS_VERDICT_PRACTICALLY_EQUIVALENT
} array_add_stats_verdict;

size_t array_add_stats_trim_count(size_t count);

const char *array_add_stats_trim_class_name(array_add_trim_class value);

bool array_add_stats_analyze_pairs(
    array_add_pair_sample *pairs,
    size_t count,
    size_t bootstrap_replicates,
    uint64_t bootstrap_seed,
    double confidence_level,
    double maximum_relative_ci_total_width,
    array_add_pair_summary *summary
);

bool array_add_stats_analyze_unpaired(
    array_add_unpaired_sample *samples,
    size_t count,
    array_add_unpaired_summary *summary
);

bool array_add_stats_compare_unpaired(
    array_add_unpaired_sample *baseline,
    size_t baseline_count,
    array_add_unpaired_sample *candidate,
    size_t candidate_count,
    size_t bootstrap_replicates,
    uint64_t bootstrap_seed,
    double confidence_level,
    double maximum_absolute_ci_total_width,
    array_add_unpaired_comparison *comparison
);

array_add_stats_verdict array_add_stats_classify_reduction(
    const array_add_unpaired_comparison *comparison,
    double minimum_practical_reduction,
    double equivalence_margin
);

const char *array_add_stats_verdict_name(array_add_stats_verdict verdict);

#endif
