#include "array_add_stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct ranked_value {
    size_t index;
    size_t sample_id;
    double key;
} ranked_value;

static bool checked_array_bytes(size_t count, size_t element_size,
                                size_t *bytes)
{
    if (element_size != 0U && count > SIZE_MAX / element_size) {
        return false;
    }
    *bytes = count * element_size;
    return true;
}

static int compare_double(const void *lhs, const void *rhs)
{
    const double lhs_value = *(const double *)lhs;
    const double rhs_value = *(const double *)rhs;

    return (lhs_value > rhs_value) - (lhs_value < rhs_value);
}

static int compare_ranked_value(const void *lhs, const void *rhs)
{
    const ranked_value *lhs_value = lhs;
    const ranked_value *rhs_value = rhs;

    if (lhs_value->key != rhs_value->key) {
        return (lhs_value->key > rhs_value->key) -
               (lhs_value->key < rhs_value->key);
    }
    if (lhs_value->sample_id != rhs_value->sample_id) {
        return (lhs_value->sample_id > rhs_value->sample_id) -
               (lhs_value->sample_id < rhs_value->sample_id);
    }
    return (lhs_value->index > rhs_value->index) -
           (lhs_value->index < rhs_value->index);
}

static double median_sorted(const double *values, size_t count)
{
    const size_t middle = count / 2U;

    if ((count & 1U) != 0U) {
        return values[middle];
    }
    return (values[middle - 1U] / 2.0) + (values[middle] / 2.0);
}

static bool summarize_values(const double *values, size_t count,
                             double *scratch, double *mean, double *median,
                             double *mad)
{
    long double sum = 0.0L;

    for (size_t index = 0U; index < count; ++index) {
        sum += (long double)values[index];
    }
    *mean = (double)(sum / (long double)count);
    if (!isfinite(*mean)) {
        return false;
    }

    memcpy(scratch, values, count * sizeof(*scratch));
    qsort(scratch, count, sizeof(*scratch), compare_double);
    *median = median_sorted(scratch, count);

    for (size_t index = 0U; index < count; ++index) {
        scratch[index] = fabs(values[index] - *median);
        if (!isfinite(scratch[index])) {
            return false;
        }
    }
    qsort(scratch, count, sizeof(*scratch), compare_double);
    *mad = median_sorted(scratch, count);
    return isfinite(*median) && isfinite(*mad);
}

static double quantile_sorted(const double *values, size_t count,
                              double probability)
{
    const double position = probability * (double)(count - 1U);
    const size_t lower = (size_t)position;
    const size_t upper = lower + (lower + 1U < count ? 1U : 0U);
    const double fraction = position - (double)lower;

    return values[lower] + ((values[upper] - values[lower]) * fraction);
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

static array_add_trim_class trim_class_for_rank(size_t zero_based_rank,
                                                 size_t count,
                                                 size_t trim_count)
{
    if (zero_based_rank < trim_count) {
        return ARRAY_ADD_TRIM_LOW;
    }
    if (zero_based_rank >= count - trim_count) {
        return ARRAY_ADD_TRIM_HIGH;
    }
    return ARRAY_ADD_TRIM_RETAINED;
}

size_t array_add_stats_trim_count(size_t count)
{
    return (count / 100U) * 15U + ((count % 100U) * 15U) / 100U;
}

const char *array_add_stats_trim_class_name(array_add_trim_class value)
{
    switch (value) {
    case ARRAY_ADD_TRIM_LOW:
        return "low";
    case ARRAY_ADD_TRIM_RETAINED:
        return "retained";
    case ARRAY_ADD_TRIM_HIGH:
        return "high";
    }
    return "invalid";
}

bool array_add_stats_analyze_pairs(
    array_add_pair_sample *pairs,
    size_t count,
    size_t bootstrap_replicates,
    uint64_t bootstrap_seed,
    double confidence_level,
    double maximum_relative_ci_total_width,
    array_add_pair_summary *summary
)
{
    ranked_value *ranking = NULL;
    double *retained_scalar = NULL;
    double *retained_candidate = NULL;
    double *retained_log_ratios = NULL;
    double *scratch = NULL;
    double *bootstrap = NULL;
    array_add_pair_summary computed = {0};
    size_t ranking_bytes;
    size_t retained_bytes;
    size_t bootstrap_bytes;
    size_t retained_index = 0U;
    uint64_t bootstrap_state = bootstrap_seed;
    long double retained_log_sum = 0.0L;
    double retained_log_mean;
    double lower_probability;
    double upper_probability;
    bool success = false;

    if (pairs == NULL || summary == NULL || count == 0U ||
        bootstrap_replicates == 0U || !isfinite(confidence_level) ||
        confidence_level <= 0.0 || confidence_level >= 1.0 ||
        !isfinite(maximum_relative_ci_total_width) ||
        maximum_relative_ci_total_width < 0.0) {
        return false;
    }

    computed.raw_pair_count = count;
    computed.trim_count_per_tail = array_add_stats_trim_count(count);
    computed.retained_pair_count =
        count - (2U * computed.trim_count_per_tail);
    if (computed.retained_pair_count == 0U ||
        computed.retained_pair_count > (size_t)UINT64_MAX ||
        !checked_array_bytes(count, sizeof(*ranking), &ranking_bytes) ||
        !checked_array_bytes(computed.retained_pair_count,
                             sizeof(*retained_scalar), &retained_bytes) ||
        !checked_array_bytes(bootstrap_replicates, sizeof(*bootstrap),
                             &bootstrap_bytes)) {
        return false;
    }

    ranking = malloc(ranking_bytes);
    retained_scalar = malloc(retained_bytes);
    retained_candidate = malloc(retained_bytes);
    retained_log_ratios = malloc(retained_bytes);
    scratch = malloc(retained_bytes);
    bootstrap = malloc(bootstrap_bytes);
    if (ranking == NULL || retained_scalar == NULL ||
        retained_candidate == NULL || retained_log_ratios == NULL ||
        scratch == NULL || bootstrap == NULL) {
        goto cleanup;
    }

    for (size_t index = 0U; index < count; ++index) {
        if (!isfinite(pairs[index].scalar_seconds) ||
            pairs[index].scalar_seconds <= 0.0 ||
            !isfinite(pairs[index].candidate_seconds) ||
            pairs[index].candidate_seconds <= 0.0) {
            goto cleanup;
        }
        pairs[index].paired_log_ratio =
            log(pairs[index].scalar_seconds) -
            log(pairs[index].candidate_seconds);
        if (!isfinite(pairs[index].paired_log_ratio)) {
            goto cleanup;
        }
        ranking[index].index = index;
        ranking[index].sample_id = pairs[index].sample_id;
        ranking[index].key = pairs[index].paired_log_ratio;
    }
    qsort(ranking, count, sizeof(*ranking), compare_ranked_value);

    for (size_t rank = 0U; rank < count; ++rank) {
        array_add_pair_sample *pair = &pairs[ranking[rank].index];

        pair->paired_log_ratio_rank = rank + 1U;
        pair->trim_class = trim_class_for_rank(
            rank, count, computed.trim_count_per_tail);
        if (pair->trim_class == ARRAY_ADD_TRIM_RETAINED) {
            retained_scalar[retained_index] = pair->scalar_seconds;
            retained_candidate[retained_index] = pair->candidate_seconds;
            retained_log_ratios[retained_index] = pair->paired_log_ratio;
            retained_log_sum += (long double)pair->paired_log_ratio;
            ++retained_index;
        }
    }
    if (retained_index != computed.retained_pair_count) {
        goto cleanup;
    }

    if (!summarize_values(
            retained_scalar, computed.retained_pair_count, scratch,
            &computed.scalar_trimmed_mean_seconds,
            &computed.scalar_retained_median_seconds,
            &computed.scalar_retained_mad_seconds) ||
        !summarize_values(
            retained_candidate, computed.retained_pair_count, scratch,
            &computed.candidate_trimmed_mean_seconds,
            &computed.candidate_retained_median_seconds,
            &computed.candidate_retained_mad_seconds)) {
        goto cleanup;
    }

    computed.ratio_of_trimmed_means =
        computed.scalar_trimmed_mean_seconds /
        computed.candidate_trimmed_mean_seconds;
    retained_log_mean =
        (double)(retained_log_sum / (long double)computed.retained_pair_count);
    computed.paired_geomean_speedup = exp(retained_log_mean);
    computed.candidate_time_reduction_fraction =
        1.0 - (1.0 / computed.paired_geomean_speedup);
    if (!isfinite(computed.ratio_of_trimmed_means) ||
        !isfinite(computed.paired_geomean_speedup) ||
        computed.paired_geomean_speedup <= 0.0 ||
        !isfinite(computed.candidate_time_reduction_fraction)) {
        goto cleanup;
    }

    for (size_t replicate = 0U; replicate < bootstrap_replicates;
         ++replicate) {
        long double replicate_sum = 0.0L;

        for (size_t draw = 0U; draw < computed.retained_pair_count; ++draw) {
            const size_t index = (size_t)(
                splitmix64_next(&bootstrap_state) %
                (uint64_t)computed.retained_pair_count);
            replicate_sum += (long double)retained_log_ratios[index];
        }
        bootstrap[replicate] = exp((double)(
            replicate_sum / (long double)computed.retained_pair_count));
        if (!isfinite(bootstrap[replicate])) {
            goto cleanup;
        }
    }
    qsort(bootstrap, bootstrap_replicates, sizeof(*bootstrap), compare_double);

    lower_probability = (1.0 - confidence_level) / 2.0;
    upper_probability = 1.0 - lower_probability;
    computed.paired_bootstrap_ci_low = quantile_sorted(
        bootstrap, bootstrap_replicates, lower_probability);
    computed.paired_bootstrap_ci_high = quantile_sorted(
        bootstrap, bootstrap_replicates, upper_probability);
    computed.paired_bootstrap_relative_ci_total_width =
        (computed.paired_bootstrap_ci_high -
         computed.paired_bootstrap_ci_low) /
        computed.paired_geomean_speedup;
    if (!isfinite(computed.paired_bootstrap_relative_ci_total_width) ||
        computed.paired_bootstrap_relative_ci_total_width < 0.0) {
        goto cleanup;
    }
    computed.adequacy_criterion_met =
        computed.paired_bootstrap_relative_ci_total_width <=
        maximum_relative_ci_total_width;

    *summary = computed;
    success = true;

cleanup:
    free(bootstrap);
    free(scratch);
    free(retained_log_ratios);
    free(retained_candidate);
    free(retained_scalar);
    free(ranking);
    return success;
}

bool array_add_stats_analyze_unpaired(
    array_add_unpaired_sample *samples,
    size_t count,
    array_add_unpaired_summary *summary
)
{
    ranked_value *ranking = NULL;
    double *retained_values = NULL;
    double *scratch = NULL;
    array_add_unpaired_summary computed = {0};
    size_t ranking_bytes;
    size_t retained_bytes;
    size_t retained_index = 0U;
    bool success = false;

    if (samples == NULL || summary == NULL || count == 0U) {
        return false;
    }

    computed.raw_count = count;
    computed.trim_count_per_tail = array_add_stats_trim_count(count);
    computed.retained_count = count - (2U * computed.trim_count_per_tail);
    if (computed.retained_count == 0U ||
        !checked_array_bytes(count, sizeof(*ranking), &ranking_bytes) ||
        !checked_array_bytes(computed.retained_count,
                             sizeof(*retained_values), &retained_bytes)) {
        return false;
    }

    ranking = malloc(ranking_bytes);
    retained_values = malloc(retained_bytes);
    scratch = malloc(retained_bytes);
    if (ranking == NULL || retained_values == NULL || scratch == NULL) {
        goto cleanup;
    }

    for (size_t index = 0U; index < count; ++index) {
        if (!isfinite(samples[index].value) || samples[index].value <= 0.0) {
            goto cleanup;
        }
        ranking[index].index = index;
        ranking[index].sample_id = samples[index].sample_id;
        ranking[index].key = samples[index].value;
    }
    qsort(ranking, count, sizeof(*ranking), compare_ranked_value);

    for (size_t rank = 0U; rank < count; ++rank) {
        array_add_unpaired_sample *sample = &samples[ranking[rank].index];

        sample->value_rank = rank + 1U;
        sample->trim_class = trim_class_for_rank(
            rank, count, computed.trim_count_per_tail);
        if (sample->trim_class == ARRAY_ADD_TRIM_RETAINED) {
            retained_values[retained_index] = sample->value;
            ++retained_index;
        }
    }
    if (retained_index != computed.retained_count ||
        !summarize_values(retained_values, computed.retained_count, scratch,
                          &computed.trimmed_mean,
                          &computed.retained_median,
                          &computed.retained_mad)) {
        goto cleanup;
    }

    *summary = computed;
    success = true;

cleanup:
    free(scratch);
    free(retained_values);
    free(ranking);
    return success;
}

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
)
{
    array_add_unpaired_comparison computed = {0};
    double *retained_baseline = NULL;
    double *retained_candidate = NULL;
    double *bootstrap = NULL;
    size_t baseline_bytes;
    size_t candidate_bytes;
    size_t bootstrap_bytes;
    size_t baseline_index = 0U;
    size_t candidate_index = 0U;
    uint64_t bootstrap_state = bootstrap_seed;
    const double lower_probability = (1.0 - confidence_level) / 2.0;
    const double upper_probability = 1.0 - lower_probability;
    bool success = false;

    if (baseline == NULL || candidate == NULL || comparison == NULL ||
        baseline_count == 0U || candidate_count == 0U ||
        bootstrap_replicates == 0U || !isfinite(confidence_level) ||
        confidence_level <= 0.0 || confidence_level >= 1.0 ||
        !isfinite(maximum_absolute_ci_total_width) ||
        maximum_absolute_ci_total_width < 0.0 ||
        !array_add_stats_analyze_unpaired(
            baseline, baseline_count, &computed.baseline) ||
        !array_add_stats_analyze_unpaired(
            candidate, candidate_count, &computed.candidate)) {
        return false;
    }

    if (computed.baseline.retained_count > (size_t)UINT64_MAX ||
        computed.candidate.retained_count > (size_t)UINT64_MAX ||
        !checked_array_bytes(computed.baseline.retained_count,
                             sizeof(*retained_baseline), &baseline_bytes) ||
        !checked_array_bytes(computed.candidate.retained_count,
                             sizeof(*retained_candidate), &candidate_bytes) ||
        !checked_array_bytes(bootstrap_replicates, sizeof(*bootstrap),
                             &bootstrap_bytes)) {
        return false;
    }

    retained_baseline = malloc(baseline_bytes);
    retained_candidate = malloc(candidate_bytes);
    bootstrap = malloc(bootstrap_bytes);
    if (retained_baseline == NULL || retained_candidate == NULL ||
        bootstrap == NULL) {
        goto cleanup;
    }

    for (size_t index = 0U; index < baseline_count; ++index) {
        if (baseline[index].trim_class == ARRAY_ADD_TRIM_RETAINED) {
            retained_baseline[baseline_index] = baseline[index].value;
            ++baseline_index;
        }
    }
    for (size_t index = 0U; index < candidate_count; ++index) {
        if (candidate[index].trim_class == ARRAY_ADD_TRIM_RETAINED) {
            retained_candidate[candidate_index] = candidate[index].value;
            ++candidate_index;
        }
    }
    if (baseline_index != computed.baseline.retained_count ||
        candidate_index != computed.candidate.retained_count) {
        goto cleanup;
    }

    computed.candidate_to_baseline_trimmed_mean_ratio =
        computed.candidate.trimmed_mean / computed.baseline.trimmed_mean;
    computed.relative_reduction =
        1.0 - computed.candidate_to_baseline_trimmed_mean_ratio;
    if (!isfinite(computed.candidate_to_baseline_trimmed_mean_ratio) ||
        !isfinite(computed.relative_reduction)) {
        goto cleanup;
    }

    for (size_t replicate = 0U; replicate < bootstrap_replicates;
         ++replicate) {
        long double baseline_sum = 0.0L;
        long double candidate_sum = 0.0L;

        for (size_t draw = 0U; draw < computed.baseline.retained_count;
             ++draw) {
            const size_t index = (size_t)(
                splitmix64_next(&bootstrap_state) %
                (uint64_t)computed.baseline.retained_count);
            baseline_sum += (long double)retained_baseline[index];
        }
        for (size_t draw = 0U; draw < computed.candidate.retained_count;
             ++draw) {
            const size_t index = (size_t)(
                splitmix64_next(&bootstrap_state) %
                (uint64_t)computed.candidate.retained_count);
            candidate_sum += (long double)retained_candidate[index];
        }

        const long double baseline_mean =
            baseline_sum / (long double)computed.baseline.retained_count;
        const long double candidate_mean =
            candidate_sum / (long double)computed.candidate.retained_count;

        bootstrap[replicate] =
            1.0 - (double)(candidate_mean / baseline_mean);
        if (!isfinite(bootstrap[replicate])) {
            goto cleanup;
        }
    }
    qsort(bootstrap, bootstrap_replicates, sizeof(*bootstrap), compare_double);

    computed.bootstrap_ci_low = quantile_sorted(
        bootstrap, bootstrap_replicates, lower_probability);
    computed.bootstrap_ci_high = quantile_sorted(
        bootstrap, bootstrap_replicates, upper_probability);
    computed.bootstrap_ci_absolute_total_width =
        computed.bootstrap_ci_high - computed.bootstrap_ci_low;
    if (!isfinite(computed.bootstrap_ci_low) ||
        !isfinite(computed.bootstrap_ci_high) ||
        !isfinite(computed.bootstrap_ci_absolute_total_width) ||
        computed.bootstrap_ci_absolute_total_width < 0.0) {
        goto cleanup;
    }
    computed.adequacy_criterion_met =
        computed.bootstrap_ci_absolute_total_width <=
        maximum_absolute_ci_total_width;

    *comparison = computed;
    success = true;

cleanup:
    free(bootstrap);
    free(retained_candidate);
    free(retained_baseline);
    return success;
}

array_add_stats_verdict array_add_stats_classify_reduction(
    const array_add_unpaired_comparison *comparison,
    double minimum_practical_reduction,
    double equivalence_margin
)
{
    if (comparison == NULL || !comparison->adequacy_criterion_met ||
        !isfinite(comparison->bootstrap_ci_low) ||
        !isfinite(comparison->bootstrap_ci_high) ||
        comparison->bootstrap_ci_low > comparison->bootstrap_ci_high ||
        !isfinite(minimum_practical_reduction) ||
        minimum_practical_reduction < 0.0 || !isfinite(equivalence_margin) ||
        equivalence_margin < 0.0) {
        return ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE;
    }
    if (comparison->bootstrap_ci_low >= minimum_practical_reduction) {
        return ARRAY_ADD_STATS_VERDICT_IMPROVED;
    }
    if (comparison->bootstrap_ci_high <= -minimum_practical_reduction) {
        return ARRAY_ADD_STATS_VERDICT_REGRESSED;
    }
    if (comparison->bootstrap_ci_low >= -equivalence_margin &&
        comparison->bootstrap_ci_high <= equivalence_margin) {
        return ARRAY_ADD_STATS_VERDICT_PRACTICALLY_EQUIVALENT;
    }
    return ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE;
}

const char *array_add_stats_verdict_name(array_add_stats_verdict verdict)
{
    switch (verdict) {
    case ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE:
        return "inconclusive";
    case ARRAY_ADD_STATS_VERDICT_IMPROVED:
        return "improved";
    case ARRAY_ADD_STATS_VERDICT_REGRESSED:
        return "regressed";
    case ARRAY_ADD_STATS_VERDICT_PRACTICALLY_EQUIVALENT:
        return "practically_equivalent";
    }
    return "invalid";
}
