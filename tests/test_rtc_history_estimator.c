#include "rtc_history.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); return false; } } while (0)
#define HOUR_US (UINT64_C(3600) * 1000000)
#define BASE_US (RTC_HISTORY_UTC_MIN_US + UINT64_C(366) * 24 * HOUR_US)

static rtc_history_record_t record(uint64_t sequence, int32_t ppm, uint64_t bridge,
    uint64_t uncertainty)
{
    rtc_history_record_t value = {
        .source = 2, .sequence = sequence, .epoch = 1,
        .utc_end_us = BASE_US + sequence * (bridge + HOUR_US),
        .raw = {bridge + (int64_t)bridge * ppm / 1000000, 0},
        .awake_delta_us = HOUR_US, .utc_delta_us = bridge + HOUR_US,
        .combined_uncertainty_us = uncertainty
    };
    if (!rtc_history_record_valid(&value)) exit(2);
    return value;
}

static bool empty_and_single(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, BASE_US, -870, &workspace, &estimate));
    CHECK(estimate.version == RTC_HISTORY_ESTIMATE_VERSION);
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_PENDING && !estimate.samples);
    CHECK(!estimate.holdout_present && !estimate.holdout_improved);
    CHECK(estimate.ppm == -870);
    rtc_history_record_t r = record(1, -800, 12 * HOUR_US, 1);
    CHECK(rtc_history_append(&history, &r));
    CHECK(rtc_history_estimate(&history, 2, r.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.samples == 1 && estimate.inliers == 1);
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_PENDING);
    CHECK(fabs(estimate.ppm + 800) < 0.000001);
    return true;
}

static bool weighted_exposure_and_recency(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_record_t first = record(1, 1000, 12 * HOUR_US, 0);
    rtc_history_record_t second = record(2, 1100, 24 * HOUR_US, 0);
    second.utc_end_us = first.utc_end_us + RTC_HISTORY_RECENCY_HALF_LIFE_US;
    CHECK(rtc_history_append(&history, &first));
    CHECK(rtc_history_append(&history, &second));
    rtc_history_t before = history;
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, second.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY);
    CHECK(estimate.samples == 2 && estimate.inliers == 2 && !estimate.holdout_present);
    CHECK(fabs(estimate.ppm - 1080) < 0.000001);
    CHECK(memcmp(&before, &history, sizeof history) == 0);
    return true;
}

static bool outliers_and_holdout(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    const int rates[] = {-800, -810, 3000, -805};
    for (unsigned i = 0; i < 4; ++i) {
        rtc_history_record_t r = record(i + 1, rates[i], 12 * HOUR_US, 1);
        CHECK(rtc_history_append(&history, &r));
    }
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, history.rows[3].utc_end_us,
        -870, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY);
    CHECK(estimate.samples == 4 && estimate.inliers == 2);
    CHECK(estimate.ppm > -810 && estimate.ppm < -800);
    CHECK(estimate.holdout_present && estimate.holdout_improved);
    CHECK(estimate.holdout_candidate_error_us < estimate.holdout_baseline_error_us);
    return true;
}

static bool newest_is_not_fitted(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    for (unsigned i = 0; i < 3; ++i) {
        rtc_history_record_t r = record(i + 1, i == 2 ? -900 : -800, 12 * HOUR_US, 1);
        CHECK(rtc_history_append(&history, &r));
    }
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, history.rows[2].utc_end_us,
        -1000, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY && estimate.inliers == 2);
    CHECK(fabs(estimate.ppm + 800) < 0.000001);
    CHECK(estimate.holdout_present && !estimate.holdout_improved);
    return true;
}

static bool conflicting_evidence(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_record_t first = record(1, -800, 12 * HOUR_US, 1);
    rtc_history_record_t second = record(2, -1400, 12 * HOUR_US, 1);
    CHECK(rtc_history_append(&history, &first));
    CHECK(rtc_history_append(&history, &second));
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, second.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_INCONSISTENT && estimate.inliers == 1);
    CHECK(!estimate.holdout_improved);
    return true;
}

static bool source_age_and_overlap(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_record_t first = record(1, -800, 12 * HOUR_US, 1);
    rtc_history_record_t overlapping = record(2, -800, 12 * HOUR_US, 1);
    overlapping.epoch = 2;
    overlapping.utc_end_us = first.utc_end_us + 6 * HOUR_US;
    rtc_history_record_t last = record(3, -800, 12 * HOUR_US, 1);
    CHECK(rtc_history_append(&history, &first));
    CHECK(rtc_history_append(&history, &overlapping));
    CHECK(rtc_history_append(&history, &last));
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, last.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.samples == 2 && estimate.excluded == 1 && estimate.inliers == 2);
    CHECK(rtc_history_estimate(&history, 0, last.utc_end_us, 0, &workspace, &estimate));
    CHECK(!estimate.samples && estimate.excluded == 3);
    CHECK(rtc_history_estimate(&history, 2,
        last.utc_end_us + RTC_HISTORY_EVIDENCE_MAX_AGE_US + 1, -870, &workspace, &estimate));
    CHECK(!estimate.samples && estimate.result == RTC_HISTORY_ESTIMATE_PENDING);
    CHECK(rtc_history_estimate(&history, 2, first.utc_end_us - 1, -870, &workspace, &estimate));
    CHECK(!estimate.samples);
    return true;
}

static bool uncertainty_does_not_shrink_with_count(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_record_t r = {0};
    for (unsigned i = 0; i < 100; ++i) {
        r = record(i + 1, -800, 12 * HOUR_US, 12 * HOUR_US / 20000);
        CHECK(rtc_history_append(&history, &r));
    }
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, r.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.samples == 100 && estimate.inliers == 99);
    CHECK(estimate.uncertainty_ppm >= 49.999999);
    CHECK(!estimate.holdout_improved);
    return true;
}

static bool invalid_inputs_are_atomic(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    memset(&estimate, 0x5a, sizeof estimate);
    const rtc_history_estimate_t before = estimate;
    CHECK(!rtc_history_estimate(NULL, 2, BASE_US, -870, &workspace, &estimate));
    CHECK(!rtc_history_estimate(&history, 1, BASE_US, -870, &workspace, &estimate));
    CHECK(!rtc_history_estimate(&history, 2, BASE_US, -20001, &workspace, &estimate));
    CHECK(!rtc_history_estimate(&history, 2, BASE_US, 20001, &workspace, &estimate));
    CHECK(!rtc_history_estimate(&history, 2, BASE_US, -870, NULL, &estimate));
    CHECK(!rtc_history_estimate(&history, 2, BASE_US, -870, &workspace, NULL));
    CHECK(!rtc_history_estimate(&history, 2, 0, -870, &workspace, &estimate));
    rtc_history_record_t r = record(1, -800, 12 * HOUR_US, 1);
    CHECK(rtc_history_append(&history, &r));
    history.rows[0].raw.fraction_q19 = RTC_DRIFT_Q19;
    CHECK(!rtc_history_estimate(&history, 2, r.utc_end_us, -870, &workspace, &estimate));
    CHECK(memcmp(&before, &estimate, sizeof estimate) == 0);
    return true;
}

static bool reference_quality_weights(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_record_t first = record(1, 1000, 12 * HOUR_US, 12 * HOUR_US / 1000000);
    rtc_history_record_t second = record(2, 1100, 12 * HOUR_US, 12 * HOUR_US * 80 / 1000000);
    CHECK(rtc_history_append(&history, &first));
    CHECK(rtc_history_append(&history, &second));
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    CHECK(rtc_history_estimate(&history, 2, second.utc_end_us, -870, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY);
    CHECK(estimate.ppm > 1000 && estimate.ppm < 1005);
    CHECK(estimate.uncertainty_ppm >= 1.0);
    return true;
}

static bool fractional_rates_and_exact_limits(void)
{
    rtc_history_t history;
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    for (int limit = -20000; limit <= 20000; limit += 40000) {
        rtc_history_init(&history);
        for (unsigned i = 0; i < 3; ++i) {
            rtc_history_record_t r = record(i + 1, limit, 12 * HOUR_US, 0);
            CHECK(rtc_history_append(&history, &r));
        }
        CHECK(rtc_history_estimate(&history, 2, history.rows[2].utc_end_us,
            0, &workspace, &estimate));
        CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY);
        CHECK(fabs(estimate.ppm - limit) < 0.000001);
    }
    rtc_history_init(&history);
    rtc_history_record_t first = record(1, 1000, 12 * HOUR_US, 1);
    rtc_history_record_t second = record(2, 1000, 12 * HOUR_US, 1);
    first.raw.us += 17280;
    first.raw.fraction_q19 = RTC_DRIFT_Q19 / 2;
    second.raw.us += 34560;
    CHECK(rtc_history_append(&history, &first));
    CHECK(rtc_history_append(&history, &second));
    CHECK(rtc_history_estimate(&history, 2, second.utc_end_us, 0, &workspace, &estimate));
    CHECK(estimate.ppm > 1000.4 && estimate.ppm < 1000.8);
    CHECK(fabs(estimate.ppm - round(estimate.ppm)) > 0.05);
    return true;
}

static bool heterogeneous_endpoint_weights(void)
{
    rtc_history_t history;
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    for (int limit = -20000; limit <= 20000; limit += 40000) {
        for (unsigned count = 3; count <= RTC_HISTORY_CAPACITY; ++count) {
            rtc_history_init(&history);
            uint64_t end = BASE_US;
            for (unsigned i = 0; i < count; ++i) {
                const uint64_t bridge = (12 + i % 7) * HOUR_US + i * 1000;
                rtc_history_record_t r = record(i + 1, limit, bridge,
                    bridge * ((i * 17) % 90) / 1000000);
                end += r.utc_delta_us;
                r.utc_end_us = end;
                CHECK(rtc_history_append(&history, &r));
            }
            if (!rtc_history_estimate(&history, 2, end + count * 1000000,
                    0, &workspace, &estimate)) {
                fprintf(stderr, "valid endpoint %d rejected with %u rows\n", limit, count);
                return false;
            }
            CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY);
            CHECK(fabs(estimate.ppm - limit) < 0.000001);
        }
    }
    return true;
}

static bool source_zero_and_inconsistent_holdout(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    for (unsigned i = 0; i < 2; ++i) {
        rtc_history_record_t r = record(i + 1, 1000 + (int)i * 10, 12 * HOUR_US, 1);
        r.source = 0;
        CHECK(rtc_history_append(&history, &r));
    }
    CHECK(rtc_history_estimate(&history, 0, history.rows[1].utc_end_us,
        0, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_READY && estimate.source == 0);
    CHECK(estimate.ppm > 1000 && estimate.ppm < 1010);
    rtc_history_init(&history);
    for (unsigned i = 0; i < 3; ++i) {
        rtc_history_record_t r = record(i + 1, i == 2 ? -1400 : -800, 12 * HOUR_US, 1);
        CHECK(rtc_history_append(&history, &r));
    }
    CHECK(rtc_history_estimate(&history, 2, history.rows[2].utc_end_us,
        -870, &workspace, &estimate));
    CHECK(estimate.result == RTC_HISTORY_ESTIMATE_INCONSISTENT);
    CHECK(estimate.holdout_present && !estimate.holdout_improved);
    CHECK(fabs(estimate.ppm + 800) < 0.000001);
    return true;
}

static bool recent_independent_recovery(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        rtc_history_t history;
        rtc_history_init(&history);
        const int rates[] = {842, 7165, 7989};
        for (unsigned i = 0; i < 3; ++i) {
            rtc_history_record_t r = record(i + 1, sign * rates[i], 25 * HOUR_US,
                (i == 1 ? 33 : 16) * 90000);
            CHECK(rtc_history_append(&history, &r));
        }
        rtc_history_estimate_workspace_t workspace;
        rtc_history_estimate_t estimate;
        CHECK(rtc_history_estimate(&history, 2, history.rows[2].utc_end_us,
            sign * 870, &workspace, &estimate));
        CHECK(estimate.holdout_present && estimate.holdout_improved);
        /* The newest record validates the candidate; it must not fit it. */
        CHECK(fabs(estimate.ppm - sign * 7165) < 0.000001);
        CHECK(estimate.result == RTC_HISTORY_ESTIMATE_RECOVERY);
        CHECK(estimate.uncertainty_ppm >= 824 + 33 + 16);
        CHECK(estimate.holdout_candidate_error_us < estimate.holdout_baseline_error_us / 2);
        const double promoted = estimate.ppm;
        CHECK(rtc_history_estimate(&history, 2, history.rows[2].utc_end_us,
            (int32_t)promoted, &workspace, &estimate));
        CHECK(!estimate.holdout_improved); /* Repeated reads cannot keep moving the rate. */
    }
    return true;
}

static bool recovery_rejects_unsafe_evidence(void)
{
    const int pairs[][2] = {
        {-800, -8000},       /* One large deviation is insufficient. */
        {-7100, 7000},       /* Opposite residual directions. */
        {-19000, -6000},     /* Candidate makes the holdout worse. */
        {-3000, -8000},      /* Less than 50% improvement. */
        {-6000, -8000},      /* Nominal 72% improvement fails after the variation margin. */
        {-1100, -1500}       /* Small improvement buried by rate variation. */
    };
    for (size_t p = 0; p < sizeof pairs / sizeof pairs[0]; ++p) {
        rtc_history_t history;
        rtc_history_init(&history);
        for (unsigned i = 0; i < 2; ++i) {
            rtc_history_record_t r = record(i + 1, pairs[p][i], 25 * HOUR_US, 2000000);
            CHECK(rtc_history_append(&history, &r));
        }
        rtc_history_estimate_workspace_t workspace;
        rtc_history_estimate_t estimate;
        CHECK(rtc_history_estimate(&history, 2, history.rows[1].utc_end_us,
            -870, &workspace, &estimate));
        CHECK(!estimate.holdout_improved);
    }
    return true;
}

static bool recovery_freshness_and_old_majority(void)
{
    rtc_history_t history;
    rtc_history_init(&history);
    for (unsigned i = 0; i < 30; ++i) {
        rtc_history_record_t r = record(i + 1, i < 28 ? -850 : (i == 28 ? -7165 : -7989),
            25 * HOUR_US, 1000000);
        CHECK(rtc_history_append(&history, &r));
    }
    rtc_history_estimate_workspace_t workspace;
    rtc_history_estimate_t estimate;
    const uint64_t now = history.rows[29].utc_end_us;
    CHECK(rtc_history_estimate(&history, 2, now, -870, &workspace, &estimate));
    CHECK(estimate.holdout_improved && fabs(estimate.ppm + 7165) < 0.000001);
    CHECK(rtc_history_estimate(&history, 2, now + 8 * 24 * HOUR_US,
        -870, &workspace, &estimate));
    CHECK(!estimate.holdout_improved);
    CHECK(rtc_history_estimate(&history, 0, now, -870, &workspace, &estimate));
    CHECK(!estimate.holdout_improved && !estimate.samples);
    return true;
}

static bool recovery_cannot_skip_newest_evidence(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        rtc_history_t history;
        rtc_history_init(&history);
        rtc_history_record_t first = record(1, -7165, 25 * HOUR_US, 1000000);
        rtc_history_record_t second = record(2, -7989, 25 * HOUR_US, 1000000);
        rtc_history_record_t third = record(3, -870, 25 * HOUR_US, 1000000);
        third.epoch = 2;
        third.utc_end_us -= 6 * HOUR_US; /* Valid cross-epoch overlap with second. */
        if (kind == 2) {
            second = record(2, -870, 25 * HOUR_US, 1000000);
            second.epoch = 2;
            second.utc_end_us -= 6 * HOUR_US;
            third = record(3, -7989, 25 * HOUR_US, 1000000);
            third.utc_end_us = second.utc_end_us + third.utc_delta_us;
        }
        CHECK(rtc_history_append(&history, &first));
        CHECK(rtc_history_append(&history, &second));
        CHECK(rtc_history_append(&history, &third));
        rtc_history_estimate_workspace_t workspace;
        rtc_history_estimate_t estimate;
        CHECK(rtc_history_estimate(&history, 2,
            kind == 1 ? second.utc_end_us : third.utc_end_us, -870, &workspace, &estimate));
        CHECK(estimate.excluded == 1 && !estimate.holdout_improved);
    }
    return true;
}

int main(int argc, char **argv)
{
    const struct { const char *name; bool (*run)(void); } tests[] = {
        {"empty_and_single", empty_and_single},
        {"weighted_exposure_and_recency", weighted_exposure_and_recency},
        {"outliers_and_holdout", outliers_and_holdout},
        {"newest_is_not_fitted", newest_is_not_fitted},
        {"conflicting_evidence", conflicting_evidence},
        {"source_age_and_overlap", source_age_and_overlap},
        {"uncertainty_does_not_shrink_with_count", uncertainty_does_not_shrink_with_count},
        {"invalid_inputs_are_atomic", invalid_inputs_are_atomic},
        {"reference_quality_weights", reference_quality_weights},
        {"fractional_rates_and_exact_limits", fractional_rates_and_exact_limits},
        {"heterogeneous_endpoint_weights", heterogeneous_endpoint_weights},
        {"source_zero_and_inconsistent_holdout", source_zero_and_inconsistent_holdout},
        {"recent_independent_recovery", recent_independent_recovery},
        {"recovery_rejects_unsafe_evidence", recovery_rejects_unsafe_evidence},
        {"recovery_freshness_and_old_majority", recovery_freshness_and_old_majority},
        {"recovery_cannot_skip_newest_evidence", recovery_cannot_skip_newest_evidence}
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
        if (argc == 2 && strcmp(argv[1], tests[i].name) != 0) continue;
        if (!tests[i].run()) return 1;
        printf("PASS %s\n", tests[i].name);
    }
    return 0;
}
