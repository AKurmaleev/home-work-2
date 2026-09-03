#include "array_add_stats.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAIR_COUNT 101U
#define BOOTSTRAP_REPLICATES 10000U
#define BOOTSTRAP_SEED UINT64_C(0x6A09E667F3BCC909)
#define CONFIDENCE_LEVEL 0.95
#define MAXIMUM_RELATIVE_CI_TOTAL_WIDTH 0.10
#define MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH 0.005
#define MINIMUM_PRACTICAL_REDUCTION 0.05
#define EQUIVALENCE_MARGIN 0.01

static size_t failure_count;

static void check_true(bool condition, const char *test_name, int line,
                       const char *expression)
{
    if (!condition) {
        fprintf(stderr, "%s:%d: check failed: %s\n", test_name, line,
                expression);
        ++failure_count;
    }
}

static bool nearly_equal(double actual, double expected,
                         double relative_tolerance)
{
    const double scale = fmax(1.0, fabs(expected));

    return isfinite(actual) &&
           fabs(actual - expected) <= relative_tolerance * scale;
}

#define CHECK(test_name, expression) \
    check_true((expression), (test_name), __LINE__, #expression)

static void test_json_double_round_trip(void)
{
    const char *test_name = "json-double-round-trip";
    const double values[] = {
        nextafter(1.0, 2.0),
        nextafter(0.000000253, INFINITY),
        0.1,
        DBL_MIN,
        DBL_MAX,
    };

    for (size_t index = 0U; index < sizeof(values) / sizeof(values[0]);
         ++index) {
        char buffer[1024];
        char *end = NULL;
        const double value = values[index];
        const int length = snprintf(buffer, sizeof(buffer),
                                    ARRAY_ADD_JSON_DOUBLE_FORMAT, value);

        CHECK(test_name, length >= 0);
        CHECK(test_name, (size_t)length < sizeof(buffer));

        const double parsed = strtod(buffer, &end);

        CHECK(test_name, end != buffer);
        CHECK(test_name, end != NULL && *end == '\0');
        CHECK(test_name, memcmp(&parsed, &value, sizeof(value)) == 0);
    }
}

static void test_trim_counts(void)
{
    const char *test_name = "trim-counts";

    CHECK(test_name, array_add_stats_trim_count(6U) == 0U);
    CHECK(test_name, array_add_stats_trim_count(7U) == 1U);
    CHECK(test_name, array_add_stats_trim_count(29U) == 4U);
    CHECK(test_name, array_add_stats_trim_count(31U) == 4U);
    CHECK(test_name, array_add_stats_trim_count(101U) == 15U);
}

static void test_whole_pair_trim(void)
{
    const char *test_name = "whole-pair-trim";
    array_add_pair_sample pairs[PAIR_COUNT] = {0};
    array_add_pair_summary summary = {0};

    for (size_t index = 0U; index < PAIR_COUNT; ++index) {
        pairs[index].sample_id = index + 1U;
        pairs[index].ab_order = (index & 1U) == 0U;
        pairs[index].scalar_seconds = (double)(index + 1U);
        pairs[index].candidate_seconds = 1.0;
    }

    CHECK(test_name,
          array_add_stats_analyze_pairs(
              pairs, PAIR_COUNT, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
              CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary));
    CHECK(test_name, summary.raw_pair_count == 101U);
    CHECK(test_name, summary.trim_count_per_tail == 15U);
    CHECK(test_name, summary.retained_pair_count == 71U);

    for (size_t index = 0U; index < PAIR_COUNT; ++index) {
        const size_t sample_id = index + 1U;
        const array_add_trim_class expected =
            sample_id <= 15U ? ARRAY_ADD_TRIM_LOW :
            sample_id <= 86U ? ARRAY_ADD_TRIM_RETAINED :
                               ARRAY_ADD_TRIM_HIGH;

        CHECK(test_name, pairs[index].paired_log_ratio_rank == sample_id);
        CHECK(test_name, pairs[index].trim_class == expected);
    }
}

static void test_pair_ties(void)
{
    const char *test_name = "pair-ties";
    array_add_pair_sample pairs[PAIR_COUNT] = {0};
    array_add_pair_summary summary = {0};

    for (size_t index = 0U; index < PAIR_COUNT; ++index) {
        pairs[index].sample_id = PAIR_COUNT - index;
        pairs[index].scalar_seconds = 2.0;
        pairs[index].candidate_seconds = 1.0;
    }

    CHECK(test_name,
          array_add_stats_analyze_pairs(
              pairs, PAIR_COUNT, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
              CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary));
    for (size_t index = 0U; index < PAIR_COUNT; ++index) {
        const size_t sample_id = pairs[index].sample_id;
        const array_add_trim_class expected =
            sample_id <= 15U ? ARRAY_ADD_TRIM_LOW :
            sample_id <= 86U ? ARRAY_ADD_TRIM_RETAINED :
                               ARRAY_ADD_TRIM_HIGH;

        CHECK(test_name, pairs[index].paired_log_ratio_rank == sample_id);
        CHECK(test_name, pairs[index].trim_class == expected);
    }
}

static void fill_shared_retention_pairs(array_add_pair_sample *pairs)
{
    for (size_t index = 0U; index < PAIR_COUNT; ++index) {
        const size_t sample_id = index + 1U;

        pairs[index].sample_id = sample_id;
        pairs[index].candidate_seconds = 1.0;
        if (sample_id <= 15U) {
            pairs[index].scalar_seconds = 0.25;
        } else if (sample_id <= 86U) {
            pairs[index].scalar_seconds = 2.0;
        } else {
            pairs[index].scalar_seconds = 8.0;
        }
    }
}

static void test_shared_pair_retention(void)
{
    const char *test_name = "shared-pair-retention";
    array_add_pair_sample pairs[PAIR_COUNT] = {0};
    array_add_pair_summary summary = {0};

    fill_shared_retention_pairs(pairs);
    CHECK(test_name,
          array_add_stats_analyze_pairs(
              pairs, PAIR_COUNT, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
              CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary));
    CHECK(test_name,
          nearly_equal(summary.scalar_trimmed_mean_seconds, 2.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.scalar_retained_median_seconds, 2.0,
                       1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.scalar_retained_mad_seconds, 0.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.candidate_trimmed_mean_seconds, 1.0,
                       1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.candidate_retained_median_seconds, 1.0,
                       1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.candidate_retained_mad_seconds, 0.0,
                       1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.ratio_of_trimmed_means, 2.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.paired_geomean_speedup, 2.0, 1.0e-12));
}

static void test_retained_statistics(void)
{
    const char *test_name = "retained-statistics";
    array_add_pair_sample pairs[7] = {0};
    array_add_pair_summary summary = {0};
    const double scalar_values[7] = {0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    const double expected_paired_geomean_speedup = pow(120.0, 1.0 / 5.0);
    const double expected_candidate_time_reduction_fraction =
        1.0 - (1.0 / expected_paired_geomean_speedup);

    for (size_t index = 0U; index < 7U; ++index) {
        pairs[index].sample_id = index + 1U;
        pairs[index].scalar_seconds = scalar_values[index];
        pairs[index].candidate_seconds = 1.0;
    }

    CHECK(test_name,
          array_add_stats_analyze_pairs(
              pairs, 7U, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
              CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary));
    CHECK(test_name, summary.trim_count_per_tail == 1U);
    CHECK(test_name, summary.retained_pair_count == 5U);
    CHECK(test_name,
          nearly_equal(summary.scalar_trimmed_mean_seconds, 3.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.scalar_retained_median_seconds, 3.0,
                       1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.scalar_retained_mad_seconds, 1.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.paired_geomean_speedup,
                       expected_paired_geomean_speedup, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.candidate_time_reduction_fraction,
                       expected_candidate_time_reduction_fraction, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.candidate_time_reduction_fraction,
                       1.0 - (1.0 / summary.paired_geomean_speedup),
                       1.0e-12));
    CHECK(test_name,
          !nearly_equal(summary.candidate_time_reduction_fraction,
                        1.0 - (1.0 / summary.ratio_of_trimmed_means),
                        1.0e-6));
}

static void test_retained_bootstrap(void)
{
    const char *test_name = "retained-bootstrap";
    array_add_pair_sample pairs[PAIR_COUNT] = {0};
    array_add_pair_summary summary = {0};

    fill_shared_retention_pairs(pairs);
    CHECK(test_name,
          array_add_stats_analyze_pairs(
              pairs, PAIR_COUNT, BOOTSTRAP_REPLICATES, BOOTSTRAP_SEED,
              CONFIDENCE_LEVEL, MAXIMUM_RELATIVE_CI_TOTAL_WIDTH, &summary));
    CHECK(test_name,
          nearly_equal(summary.paired_geomean_speedup, 2.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.paired_bootstrap_ci_low, 2.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.paired_bootstrap_ci_high, 2.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(summary.paired_bootstrap_relative_ci_total_width,
                       0.0, 1.0e-12));
    CHECK(test_name, summary.adequacy_criterion_met);
}

static void test_independent_unpaired_trim(void)
{
    const char *test_name = "independent-unpaired-trim";
    array_add_unpaired_sample scalar[31] = {0};
    array_add_unpaired_sample candidate[31] = {0};
    array_add_unpaired_summary scalar_summary = {0};
    array_add_unpaired_summary candidate_summary = {0};

    for (size_t index = 0U; index < 31U; ++index) {
        const size_t sample_id = index + 1U;

        scalar[index].sample_id = sample_id;
        scalar[index].value = (double)sample_id;
        candidate[index].sample_id = sample_id;
        candidate[index].value = (double)(((sample_id * 7U) % 31U) + 1U);
    }

    CHECK(test_name,
          array_add_stats_analyze_unpaired(scalar, 31U, &scalar_summary));
    CHECK(test_name,
          array_add_stats_analyze_unpaired(candidate, 31U,
                                           &candidate_summary));
    CHECK(test_name, scalar_summary.trim_count_per_tail == 4U);
    CHECK(test_name, scalar_summary.retained_count == 23U);
    CHECK(test_name, candidate_summary.trim_count_per_tail == 4U);
    CHECK(test_name, candidate_summary.retained_count == 23U);
    CHECK(test_name,
          nearly_equal(scalar_summary.trimmed_mean, 16.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(candidate_summary.trimmed_mean, 16.0, 1.0e-12));
    CHECK(test_name, scalar[30].trim_class == ARRAY_ADD_TRIM_HIGH);
    CHECK(test_name, candidate[30].trim_class == ARRAY_ADD_TRIM_LOW);
}

static void fill_constant_samples(array_add_unpaired_sample *samples,
                                  size_t count, double value,
                                  bool reverse_sample_ids)
{
    for (size_t index = 0U; index < count; ++index) {
        samples[index].sample_id =
            reverse_sample_ids ? count - index : index + 1U;
        samples[index].value = value;
    }
}

static void test_unpaired_comparison_ties_and_equivalence(void)
{
    const char *test_name = "unpaired-comparison-ties-and-equivalence";
    array_add_unpaired_sample baseline[31] = {0};
    array_add_unpaired_sample candidate[31] = {0};
    array_add_unpaired_comparison comparison = {0};

    fill_constant_samples(baseline, 31U, 2.0, true);
    fill_constant_samples(candidate, 31U, 2.0, false);
    for (size_t index = 0U; index < 31U; ++index) {
        candidate[index].sample_id = 1U;
    }

    CHECK(test_name,
          array_add_stats_compare_unpaired(
              baseline, 31U, candidate, 31U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    CHECK(test_name, comparison.baseline.raw_count == 31U);
    CHECK(test_name, comparison.baseline.trim_count_per_tail == 4U);
    CHECK(test_name, comparison.baseline.retained_count == 23U);
    CHECK(test_name, comparison.candidate.raw_count == 31U);
    CHECK(test_name, comparison.candidate.trim_count_per_tail == 4U);
    CHECK(test_name, comparison.candidate.retained_count == 23U);

    for (size_t index = 0U; index < 31U; ++index) {
        CHECK(test_name, baseline[index].value_rank == baseline[index].sample_id);
        CHECK(test_name, candidate[index].value_rank == index + 1U);
    }

    CHECK(test_name,
          nearly_equal(comparison.candidate_to_baseline_trimmed_mean_ratio,
                       1.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.relative_reduction, 0.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_low, 0.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_high, 0.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_absolute_total_width, 0.0,
                       1.0e-12));
    CHECK(test_name, comparison.adequacy_criterion_met);
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) ==
              ARRAY_ADD_STATS_VERDICT_PRACTICALLY_EQUIVALENT);
}

static void test_unpaired_comparison_improved(void)
{
    const char *test_name = "unpaired-comparison-improved";
    array_add_unpaired_sample baseline[31] = {0};
    array_add_unpaired_sample candidate[31] = {0};
    array_add_unpaired_comparison comparison = {0};

    fill_constant_samples(baseline, 31U, 2.0, false);
    fill_constant_samples(candidate, 31U, 1.0, false);

    CHECK(test_name,
          array_add_stats_compare_unpaired(
              baseline, 31U, candidate, 31U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    CHECK(test_name,
          nearly_equal(comparison.candidate_to_baseline_trimmed_mean_ratio,
                       0.5, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.relative_reduction, 0.5, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_low, 0.5, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_high, 0.5, 1.0e-12));
    CHECK(test_name, comparison.adequacy_criterion_met);
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) == ARRAY_ADD_STATS_VERDICT_IMPROVED);
}

static void test_unpaired_comparison_independent_distributions(void)
{
    const char *test_name = "unpaired-comparison-independent-distributions";
    array_add_unpaired_sample baseline[31] = {0};
    array_add_unpaired_sample candidate[33] = {0};
    array_add_unpaired_comparison comparison = {0};
    array_add_unpaired_comparison repeated = {0};
    array_add_unpaired_comparison permissive = {0};

    for (size_t index = 0U; index < 31U; ++index) {
        baseline[index].sample_id = index + 1U;
        baseline[index].value = (double)(index + 1U);
    }
    for (size_t index = 0U; index < 33U; ++index) {
        candidate[index].sample_id = index + 1U;
        candidate[index].value = (double)(2U * (index + 1U));
    }

    CHECK(test_name,
          array_add_stats_compare_unpaired(
              baseline, 31U, candidate, 33U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    CHECK(test_name, comparison.baseline.trim_count_per_tail == 4U);
    CHECK(test_name, comparison.baseline.retained_count == 23U);
    CHECK(test_name, comparison.candidate.trim_count_per_tail == 4U);
    CHECK(test_name, comparison.candidate.retained_count == 25U);
    CHECK(test_name,
          nearly_equal(comparison.baseline.trimmed_mean, 16.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.candidate.trimmed_mean, 34.0, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.candidate_to_baseline_trimmed_mean_ratio,
                       2.125, 1.0e-12));
    CHECK(test_name,
          nearly_equal(comparison.relative_reduction, -1.125, 1.0e-12));
    CHECK(test_name, comparison.bootstrap_ci_low < comparison.bootstrap_ci_high);
    CHECK(test_name,
          comparison.bootstrap_ci_low <= comparison.relative_reduction);
    CHECK(test_name,
          comparison.relative_reduction <= comparison.bootstrap_ci_high);
    CHECK(test_name,
          nearly_equal(comparison.bootstrap_ci_absolute_total_width,
                       comparison.bootstrap_ci_high -
                           comparison.bootstrap_ci_low,
                       1.0e-12));
    CHECK(test_name,
          comparison.adequacy_criterion_met ==
              (comparison.bootstrap_ci_absolute_total_width <=
               MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH));
    CHECK(test_name, !comparison.adequacy_criterion_met);

    CHECK(test_name,
          array_add_stats_compare_unpaired(
              baseline, 31U, candidate, 33U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &repeated));
    CHECK(test_name, repeated.bootstrap_ci_low == comparison.bootstrap_ci_low);
    CHECK(test_name, repeated.bootstrap_ci_high == comparison.bootstrap_ci_high);
    CHECK(test_name,
          repeated.bootstrap_ci_absolute_total_width ==
              comparison.bootstrap_ci_absolute_total_width);
    CHECK(test_name,
          repeated.adequacy_criterion_met ==
              comparison.adequacy_criterion_met);

    CHECK(test_name,
          array_add_stats_compare_unpaired(
              baseline, 31U, candidate, 33U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL, 2.0, &permissive));
    CHECK(test_name,
          permissive.bootstrap_ci_low == comparison.bootstrap_ci_low);
    CHECK(test_name,
          permissive.bootstrap_ci_high == comparison.bootstrap_ci_high);
    CHECK(test_name,
          permissive.bootstrap_ci_absolute_total_width ==
              comparison.bootstrap_ci_absolute_total_width);
    CHECK(test_name,
          permissive.adequacy_criterion_met ==
              (permissive.bootstrap_ci_absolute_total_width <= 2.0));
    CHECK(test_name, permissive.adequacy_criterion_met);
}

static void test_unpaired_comparison_verdicts(void)
{
    const char *test_name = "unpaired-comparison-verdicts";
    array_add_unpaired_comparison comparison = {0};

    comparison.bootstrap_ci_low = 0.05;
    comparison.bootstrap_ci_high = 0.08;
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) == ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE);

    comparison.adequacy_criterion_met = true;
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) == ARRAY_ADD_STATS_VERDICT_IMPROVED);

    comparison.bootstrap_ci_low = -0.08;
    comparison.bootstrap_ci_high = -0.05;
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) == ARRAY_ADD_STATS_VERDICT_REGRESSED);

    comparison.bootstrap_ci_low = -0.01;
    comparison.bootstrap_ci_high = 0.01;
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) ==
              ARRAY_ADD_STATS_VERDICT_PRACTICALLY_EQUIVALENT);

    comparison.bootstrap_ci_low = -0.02;
    comparison.bootstrap_ci_high = 0.01;
    CHECK(test_name,
          array_add_stats_classify_reduction(
              &comparison, MINIMUM_PRACTICAL_REDUCTION,
              EQUIVALENCE_MARGIN) == ARRAY_ADD_STATS_VERDICT_INCONCLUSIVE);
}

static void test_unpaired_comparison_rejects_invalid_input(void)
{
    const char *test_name = "unpaired-comparison-rejects-invalid-input";
    array_add_unpaired_sample baseline = {.sample_id = 1U, .value = 1.0};
    array_add_unpaired_sample candidate = {.sample_id = 1U, .value = 1.0};
    array_add_unpaired_comparison comparison = {0};

    CHECK(test_name,
          !array_add_stats_compare_unpaired(
              &baseline, 0U, &candidate, 1U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    CHECK(test_name,
          !array_add_stats_compare_unpaired(
              &baseline, SIZE_MAX, &candidate, 1U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));

    baseline.value = 0.0;
    CHECK(test_name,
          !array_add_stats_compare_unpaired(
              &baseline, 1U, &candidate, 1U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    baseline.value = NAN;
    CHECK(test_name,
          !array_add_stats_compare_unpaired(
              &baseline, 1U, &candidate, 1U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
    baseline.value = 1.0;
    candidate.value = INFINITY;
    CHECK(test_name,
          !array_add_stats_compare_unpaired(
              &baseline, 1U, &candidate, 1U, BOOTSTRAP_REPLICATES,
              BOOTSTRAP_SEED, CONFIDENCE_LEVEL,
              MAXIMUM_ABSOLUTE_CI_TOTAL_WIDTH, &comparison));
}

int main(void)
{
    test_json_double_round_trip();
    test_trim_counts();
    test_whole_pair_trim();
    test_pair_ties();
    test_shared_pair_retention();
    test_retained_statistics();
    test_retained_bootstrap();
    test_independent_unpaired_trim();
    test_unpaired_comparison_ties_and_equivalence();
    test_unpaired_comparison_improved();
    test_unpaired_comparison_independent_distributions();
    test_unpaired_comparison_verdicts();
    test_unpaired_comparison_rejects_invalid_input();

    if (failure_count != 0U) {
        fprintf(stderr, "array add stats tests failed: %zu checks\n",
                failure_count);
        return EXIT_FAILURE;
    }

    printf("array add stats tests passed\n");
    return EXIT_SUCCESS;
}
