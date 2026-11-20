#include "rtc_history.h"

#include <math.h>
#include <string.h>

void rtc_history_init(rtc_history_t *history)
{
    if (history) memset(history, 0, sizeof *history);
}

static bool history_valid(const rtc_history_t *h)
{
    return h && h->count <= RTC_HISTORY_CAPACITY && h->next < RTC_HISTORY_CAPACITY &&
        (h->count == RTC_HISTORY_CAPACITY || h->next == h->count) &&
        h->generation >= h->count && (h->count || !h->generation);
}

static const rtc_history_record_t *row_at(const rtc_history_t *h, size_t index)
{
    return &h->rows[(h->next + RTC_HISTORY_CAPACITY - h->count + index) % RTC_HISTORY_CAPACITY];
}

bool rtc_history_append(rtc_history_t *h, const rtc_history_record_t *r)
{
    if (!history_valid(h) || !rtc_history_record_valid(r) || h->generation == UINT64_MAX)
        return false;
    const uint64_t start = r->utc_end_us - r->utc_delta_us;
    if (start < h->evicted_utc_end_us) return false;
    if (h->count) {
        const rtc_history_record_t *last = row_at(h, h->count - 1);
        if (r->sequence <= last->sequence || r->utc_end_us <= last->utc_end_us) return false;
    }
    for (size_t i = 0; i < h->count; ++i) {
        const rtc_history_record_t *prior = row_at(h, i);
        if (!rtc_history_record_valid(prior) ||
            (prior->epoch == r->epoch && prior->source == r->source && start < prior->utc_end_us))
            return false;
    }
    const rtc_history_record_t value = *r;
    if (h->count == RTC_HISTORY_CAPACITY) h->evicted_utc_end_us = h->rows[h->next].utc_end_us;
    h->rows[h->next] = value;
    h->next = (h->next + 1) % RTC_HISTORY_CAPACITY;
    if (h->count < RTC_HISTORY_CAPACITY) ++h->count;
    ++h->generation;
    return true;
}

bool rtc_history_get(const rtc_history_t *h, size_t index, rtc_history_record_t *r)
{
    if (!r || !history_valid(h) || index >= h->count) return false;
    const rtc_history_record_t *value = row_at(h, index);
    if (!rtc_history_record_valid(value)) return false;
    *r = *value;
    return true;
}

bool rtc_history_page(const rtc_history_t *history, uint64_t generation, size_t cursor,
    rtc_history_page_t *page)
{
    if (!page || !history_valid(history) || generation != history->generation ||
        cursor > history->count) return false;
    rtc_history_page_t value = {
        .generation = generation, .total = history->count, .cursor = cursor
    };
    value.count = history->count - cursor;
    if (value.count > RTC_HISTORY_PAGE_SIZE) value.count = RTC_HISTORY_PAGE_SIZE;
    for (size_t i = 0; i < value.count; ++i)
        if (!rtc_history_get(history, cursor + i, &value.rows[i])) return false;
    value.cursor += value.count;
    *page = value;
    return true;
}

_Static_assert(RTC_HISTORY_WIRE_SIZE == 80, "wire size");
_Static_assert(sizeof(rtc_history_t) + sizeof(rtc_history_page_t) + 1024 <= 16384,
    "history and temporary RAM budget");

bool rtc_history_record_valid(const rtc_history_record_t *r)
{
    if (!r || (r->flags & ~15u) || (r->source != 0 && r->source != 2) ||
        !r->sequence || !r->epoch || !rtc_drift_time_valid(&r->raw) ||
        r->utc_end_us < RTC_HISTORY_UTC_MIN_US || r->utc_end_us > RTC_HISTORY_UTC_MAX_US ||
        !r->utc_delta_us || r->utc_delta_us > RTC_HISTORY_MAX_WINDOW_US ||
        r->utc_delta_us > r->utc_end_us - RTC_HISTORY_UTC_MIN_US ||
        r->awake_delta_us >= r->utc_delta_us || r->raw.us < RTC_HISTORY_MIN_RAW_US ||
        r->raw.us > RTC_HISTORY_MAX_WINDOW_US * 51 / 50 ||
        r->combined_uncertainty_us > r->raw.us / 10000) return false;
    if (r->flags & RTC_HISTORY_TEMP_VALID) {
        if (r->temperature_centi_c < -1000 || r->temperature_centi_c > 8000) return false;
    } else if (r->temperature_centi_c || r->temp_age_s) return false;
    if (r->flags & RTC_HISTORY_BATTERY_VALID) {
        if (!r->battery_mv) return false;
    } else if (r->battery_mv || r->battery_age_s) return false;
    const uint64_t bridge = r->utc_delta_us - r->awake_delta_us;
    const uint64_t fraction = (uint64_t)r->raw.fraction_q19 * 50;
    const uint64_t scaled = r->raw.us * 50 + fraction / RTC_DRIFT_Q19;
    const uint64_t upper = bridge * 51;
    return scaled >= bridge * 49 && scaled <= upper &&
           (scaled != upper || fraction % RTC_DRIFT_Q19 == 0);
}

bool rtc_history_rate_ppm(const rtc_history_record_t *r, double *ppm)
{
    if (!ppm || !rtc_history_record_valid(r)) return false;
    const uint64_t bridge = r->utc_delta_us - r->awake_delta_us;
    const double difference = r->raw.us >= bridge ? (double)(r->raw.us - bridge) :
        -(double)(bridge - r->raw.us);
    *ppm = (difference + (double)r->raw.fraction_q19 / RTC_DRIFT_Q19) * 1000000.0 / bridge;
    return true;
}

static void put(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static uint64_t take(const uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc32(const uint8_t *p, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0 - (crc & 1)));
    }
    return ~crc;
}

bool rtc_history_encode(const rtc_history_record_t *r, uint8_t *wire, size_t size)
{
    if (!wire || size != RTC_HISTORY_WIRE_SIZE || !rtc_history_record_valid(r)) return false;
    uint8_t value[RTC_HISTORY_WIRE_SIZE] = {0};
    put(value, 1, 2);
    put(value + 2, r->flags, 2);
    put(value + 4, r->source, 4);
    put(value + 8, r->sequence, 8);
    put(value + 16, r->epoch, 8);
    put(value + 24, r->utc_end_us, 8);
    put(value + 32, r->raw.us, 8);
    put(value + 40, r->awake_delta_us, 8);
    put(value + 48, r->utc_delta_us, 8);
    put(value + 56, r->combined_uncertainty_us, 8);
    put(value + 64, r->raw.fraction_q19, 4);
    put(value + 68, (uint16_t)r->temperature_centi_c, 2);
    put(value + 70, r->battery_mv, 2);
    put(value + 72, r->temp_age_s, 2);
    put(value + 74, r->battery_age_s, 2);
    put(value + 76, crc32(value, 76), 4);
    memcpy(wire, value, sizeof value);
    return true;
}

bool rtc_history_decode(const uint8_t *wire, size_t size, rtc_history_record_t *r)
{
    if (!wire || !r || size != RTC_HISTORY_WIRE_SIZE || take(wire, 2) != 1 ||
        take(wire + 76, 4) != crc32(wire, 76)) return false;
    const uint32_t temperature = (uint32_t)take(wire + 68, 2);
    rtc_history_record_t value = {
        .flags = (uint16_t)take(wire + 2, 2),
        .source = (uint32_t)take(wire + 4, 4),
        .sequence = take(wire + 8, 8),
        .epoch = take(wire + 16, 8),
        .utc_end_us = take(wire + 24, 8),
        .raw = {take(wire + 32, 8), (uint32_t)take(wire + 64, 4)},
        .awake_delta_us = take(wire + 40, 8),
        .utc_delta_us = take(wire + 48, 8),
        .combined_uncertainty_us = take(wire + 56, 8),
        .temperature_centi_c = (int16_t)(temperature <= INT16_MAX ?
            (int32_t)temperature : (int32_t)temperature - 65536),
        .battery_mv = (uint16_t)take(wire + 70, 2),
        .temp_age_s = (uint16_t)take(wire + 72, 2),
        .battery_age_s = (uint16_t)take(wire + 74, 2)
    };
    if (!rtc_history_record_valid(&value)) return false;
    *r = value;
    return true;
}

static void sort_samples(rtc_history_estimate_sample_t *samples, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        const rtc_history_estimate_sample_t value = samples[i];
        size_t j = i;
        while (j && samples[j - 1].ppm > value.ppm) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
}

static bool sample_inlier(const rtc_history_estimate_sample_t *sample,
    const rtc_history_estimate_sample_t *median)
{
    const double tolerance = fmax(200.0, sample->uncertainty_ppm + median->uncertainty_ppm);
    return fabs(sample->ppm - median->ppm) <= tolerance;
}

static bool replay_error(const rtc_history_record_t *row, int32_t ppm, double *error)
{
    rtc_drift_ratio_t carry = {0};
    rtc_drift_time_t corrected;
    if (!rtc_drift_scale_time(&row->raw, ppm, &carry, &corrected)) return false;
    *error = fabs((double)corrected.us - (double)(row->utc_delta_us - row->awake_delta_us) +
        (double)corrected.fraction_q19 / RTC_DRIFT_Q19);
    return isfinite(*error);
}

static void recent_refinement(const rtc_history_t *history, uint64_t now_utc_us,
    int32_t baseline_ppm, const rtc_history_estimate_sample_t recent[3],
    rtc_history_estimate_t *estimate)
{
    if (estimate->result != RTC_HISTORY_ESTIMATE_INCONSISTENT) return;
    const uint64_t max_age = UINT64_C(7) * 86400 * 1000000;
    rtc_history_record_t rows[3];
    for (size_t i = 0; i < 3; ++i) {
        if (!rtc_history_get(history, recent[i].row_index, &rows[i]) ||
            rows[i].utc_end_us > now_utc_us || now_utc_us - rows[i].utc_end_us > max_age) return;
    }
    /* These must be the actual newest same-source rows, including exclusions. */
    size_t remaining = 3;
    for (size_t i = history->count; i > 0 && remaining; --i) {
        if (row_at(history, i - 1)->source != rows[2].source) continue;
        if (i - 1 != recent[remaining - 1].row_index) return;
        --remaining;
    }
    if (remaining || fabs(recent[0].ppm - recent[1].ppm) >
        fmax(200.0, recent[0].uncertainty_ppm + recent[1].uncertainty_ppm)) return;

    const double weight = recent[0].weight + recent[1].weight;
    if (!isfinite(weight) || weight <= 0.0) return;
    const double candidate = round(recent[0].ppm +
        recent[1].weight / weight * (recent[1].ppm - recent[0].ppm));
    if (!isfinite(candidate) || candidate < -20000 || candidate > 20000) return;
    const double training_uncertainty = fmax(
        recent[0].uncertainty_ppm + fabs(recent[0].ppm - candidate),
        recent[1].uncertainty_ppm + fabs(recent[1].ppm - candidate));
    if (fabs(recent[2].ppm - candidate) >
        fmax(200.0, training_uncertainty + recent[2].uncertainty_ppm)) return;
    /* Cover every observed window; low weight never erases its allowance. */
    const double uncertainty = fmax(training_uncertainty,
        recent[2].uncertainty_ppm + fabs(recent[2].ppm - candidate));
    for (size_t i = 0; i < 2; ++i) {
        double old_error, new_error;
        if (!replay_error(&rows[i], baseline_ppm, &old_error) ||
            !replay_error(&rows[i], (int32_t)candidate, &new_error)) return;
        const double allowance = 2.0 * rows[i].combined_uncertainty_us +
            (double)(rows[i].utc_delta_us - rows[i].awake_delta_us) / 1000000.0;
        if (new_error > old_error + allowance) return;
    }
    double baseline_error, candidate_error;
    if (!replay_error(&rows[2], baseline_ppm, &baseline_error) ||
        !replay_error(&rows[2], (int32_t)candidate, &candidate_error)) return;
    const double bridge = (double)(rows[2].utc_delta_us - rows[2].awake_delta_us);
    const double margin = 2.0 * rows[2].combined_uncertainty_us +
        uncertainty * bridge / (1000000.0 + candidate);
    const double improvement = baseline_error - candidate_error;
    if (improvement <= margin || improvement < 0.2 * baseline_error ||
        improvement < bridge / 1000000.0) return;

    estimate->result = RTC_HISTORY_ESTIMATE_RECENT_READY;
    estimate->inliers = 2;
    estimate->ppm = candidate;
    estimate->uncertainty_ppm = uncertainty;
    estimate->holdout_present = true;
    estimate->holdout_improved = true;
    estimate->holdout_baseline_error_us = baseline_error;
    estimate->holdout_candidate_error_us = candidate_error;
}

static void recent_recovery(const rtc_history_t *history, uint64_t now_utc_us,
    int32_t baseline_ppm, const rtc_history_estimate_sample_t *training,
    const rtc_history_estimate_sample_t *holdout, rtc_history_estimate_t *estimate)
{
    const uint64_t max_age = UINT64_C(7) * 86400 * 1000000;
    rtc_history_record_t prior, latest;
    if (estimate->result != RTC_HISTORY_ESTIMATE_INCONSISTENT ||
        !rtc_history_get(history, training->row_index, &prior) ||
        !rtc_history_get(history, holdout->row_index, &latest) ||
        now_utc_us - prior.utc_end_us > max_age ||
        now_utc_us - latest.utc_end_us > max_age) return;
    /* Do not fall back to a favorable older pair when a newer same-source
     * record was excluded (future UTC or a cross-epoch overlapping window). */
    size_t expected = holdout->row_index;
    unsigned found = 0;
    for (size_t i = history->count; i > 0 && found < 2; --i) {
        const rtc_history_record_t *row = row_at(history, i - 1);
        if (row->source != latest.source) continue;
        if (i - 1 != expected) return;
        expected = training->row_index;
        ++found;
    }
    if (found != 2) return;

    const double previous_error = training->ppm - baseline_ppm;
    const double latest_error = holdout->ppm - baseline_ppm;
    /* Both windows must independently exclude the active rate in the same
     * direction. A single bad reference cannot move the coefficient. */
    if (previous_error * latest_error <= 0.0 ||
        fabs(previous_error) <= 1000.0 + training->uncertainty_ppm ||
        fabs(latest_error) <= 1000.0 + holdout->uncertainty_ppm) return;

    /* Select using only the previous window, then validate on the newest one.
     * Include observed nonstationarity; never call a variable rate a precise fit. */
    const double candidate = round(training->ppm);
    const double uncertainty = fmax(200.0, fabs(holdout->ppm - training->ppm) +
        training->uncertainty_ppm + holdout->uncertainty_ppm + fabs(candidate - training->ppm));
    const double bridge = (double)(latest.utc_delta_us - latest.awake_delta_us);
    const double raw = (double)latest.raw.us + (double)latest.raw.fraction_q19 / RTC_DRIFT_Q19;
    const double baseline_error = fabs(raw * 1000000.0 / (1000000.0 + baseline_ppm) - bridge);
    const double candidate_error = fabs(raw * 1000000.0 / (1000000.0 + candidate) - bridge);
    const double margin = 2.0 * latest.combined_uncertainty_us +
        uncertainty * bridge / (1000000.0 + candidate);
    if (baseline_error - candidate_error <= margin ||
        candidate_error + margin >= baseline_error * 0.5) return;

    estimate->result = RTC_HISTORY_ESTIMATE_RECOVERY;
    estimate->inliers = 1; /* One training window; the holdout is not fitted. */
    estimate->ppm = candidate;
    estimate->uncertainty_ppm = uncertainty;
    estimate->holdout_present = true;
    estimate->holdout_improved = true;
    estimate->holdout_baseline_error_us = baseline_error;
    estimate->holdout_candidate_error_us = candidate_error;
}

bool rtc_history_estimate(const rtc_history_t *history, uint32_t source,
    uint64_t now_utc_us, int32_t baseline_ppm, rtc_history_estimate_workspace_t *workspace,
    rtc_history_estimate_t *estimate)
{
    if (!estimate || !workspace || !history_valid(history) || (source != 0 && source != 2) ||
        now_utc_us < RTC_HISTORY_UTC_MIN_US || now_utc_us > RTC_HISTORY_UTC_MAX_US ||
        baseline_ppm < -20000 || baseline_ppm > 20000) return false;
    rtc_history_estimate_t value = {
        .version = RTC_HISTORY_ESTIMATE_VERSION, .source = source,
        .result = RTC_HISTORY_ESTIMATE_PENDING, .ppm = baseline_ppm
    };
    uint64_t prior_end = 0;
    for (size_t i = 0; i < history->count; ++i) {
        rtc_history_record_t row;
        if (!rtc_history_get(history, i, &row)) return false;
        const uint64_t start = row.utc_end_us - row.utc_delta_us;
        if (row.source != source || row.utc_end_us > now_utc_us ||
            now_utc_us - row.utc_end_us > RTC_HISTORY_EVIDENCE_MAX_AGE_US || start < prior_end) {
            ++value.excluded;
            continue;
        }
        prior_end = row.utc_end_us;
        const uint64_t bridge = row.utc_delta_us - row.awake_delta_us;
        rtc_history_estimate_sample_t *sample = &workspace->samples[value.samples];
        if (!rtc_history_rate_ppm(&row, &sample->ppm)) return false;
        sample->uncertainty_ppm = (double)row.combined_uncertainty_us * 1000000.0 / bridge;
        const double recency = exp2(-(double)(now_utc_us - row.utc_end_us) /
            (double)RTC_HISTORY_RECENCY_HALF_LIFE_US);
        sample->weight = (double)bridge * recency / (1.0 + sample->uncertainty_ppm);
        sample->row_index = i;
        value.evidence_sequence = row.sequence;
        value.evidence_utc_us = row.utc_end_us;
        ++value.samples;
    }
    if (!value.samples) {
        *estimate = value;
        return true;
    }
    if (value.samples == 1) {
        value.ppm = workspace->samples[0].ppm;
        value.uncertainty_ppm = workspace->samples[0].uncertainty_ppm;
        value.inliers = 1;
        *estimate = value;
        return true;
    }
    /* Preserve chronological endpoints before the weighted-median sort. */
    const rtc_history_estimate_sample_t previous = workspace->samples[value.samples - 2];
    const rtc_history_estimate_sample_t latest = workspace->samples[value.samples - 1];
    rtc_history_estimate_sample_t recent[3];
    if (value.samples >= 3)
        memcpy(recent, &workspace->samples[value.samples - 3], sizeof recent);
    value.holdout_present = value.samples >= 3;
    const size_t training = value.samples - (value.holdout_present ? 1 : 0);
    sort_samples(workspace->samples, training);
    double total_weight = 0.0;
    for (size_t i = 0; i < training; ++i) total_weight += workspace->samples[i].weight;
    double cumulative = 0.0;
    size_t median_index = 0;
    for (size_t i = 0; i < training; ++i) {
        cumulative += workspace->samples[i].weight;
        median_index = i;
        if (cumulative >= total_weight / 2.0) break;
    }
    const rtc_history_estimate_sample_t median = workspace->samples[median_index];
    double weighted_delta = 0.0;
    double inlier_weight = 0.0;
    double minimum_rate = median.ppm;
    double maximum_rate = median.ppm;
    for (size_t i = 0; i < training; ++i) {
        const rtc_history_estimate_sample_t *sample = &workspace->samples[i];
        if (!sample_inlier(sample, &median)) continue;
        weighted_delta += sample->weight * (sample->ppm - median.ppm);
        inlier_weight += sample->weight;
        minimum_rate = fmin(minimum_rate, sample->ppm);
        maximum_rate = fmax(maximum_rate, sample->ppm);
        ++value.inliers;
    }
    if (!value.inliers || !isfinite(inlier_weight) || inlier_weight <= 0.0) return false;
    value.ppm = fmax(minimum_rate, fmin(maximum_rate,
        median.ppm + weighted_delta / inlier_weight));
    for (size_t i = 0; i < training; ++i) {
        const rtc_history_estimate_sample_t *sample = &workspace->samples[i];
        if (!sample_inlier(sample, &median)) continue;
        value.uncertainty_ppm += sample->weight *
            (sample->uncertainty_ppm + fabs(sample->ppm - value.ppm)) / inlier_weight;
    }
    value.result = value.inliers >= 2 ? RTC_HISTORY_ESTIMATE_READY :
        RTC_HISTORY_ESTIMATE_INCONSISTENT;
    if (value.holdout_present) {
        const rtc_history_estimate_sample_t *sample = &workspace->samples[training];
        rtc_history_record_t row;
        if (!rtc_history_get(history, sample->row_index, &row)) return false;
        const uint64_t bridge = row.utc_delta_us - row.awake_delta_us;
        const double raw = (double)row.raw.us + (double)row.raw.fraction_q19 / RTC_DRIFT_Q19;
        value.holdout_baseline_error_us = fabs(raw * 1000000.0 / (1000000.0 + baseline_ppm) - bridge);
        value.holdout_candidate_error_us = fabs(raw * 1000000.0 / (1000000.0 + value.ppm) - bridge);
        const double tolerance = fmax(200.0, sample->uncertainty_ppm + value.uncertainty_ppm);
        if (fabs(sample->ppm - value.ppm) > tolerance)
            value.result = RTC_HISTORY_ESTIMATE_INCONSISTENT;
        const double improvement = value.holdout_baseline_error_us - value.holdout_candidate_error_us;
        const double margin = 2.0 * row.combined_uncertainty_us +
            value.uncertainty_ppm * bridge / 1000000.0;
        value.holdout_improved = value.result == RTC_HISTORY_ESTIMATE_READY &&
            improvement > margin && improvement >= 0.2 * value.holdout_baseline_error_us &&
            improvement >= (double)bridge / 1000000.0;
    }
    if (!isfinite(value.ppm) || !isfinite(value.uncertainty_ppm) ||
        value.ppm < -20000.0 || value.ppm > 20000.0) return false;
    if (value.samples >= 3) recent_refinement(history, now_utc_us, baseline_ppm, recent, &value);
    recent_recovery(history, now_utc_us, baseline_ppm, &previous, &latest, &value);
    *estimate = value;
    return true;
}

_Static_assert(sizeof(rtc_history_t) + sizeof(rtc_history_page_t) +
    sizeof(rtc_history_estimate_workspace_t) + sizeof(rtc_history_estimate_t) + 1024 <= 16384,
    "history estimator RAM budget");
