#include "rtc_drift.h"

#include <math.h>

#define RTC_DRIFT_PPM_LIMIT 20000
#define RTC_DRIFT_MIN_EXPOSURE_US (UINT64_C(6) * 3600 * 1000000)
#define RTC_DRIFT_MAX_REFERENCE_US (UINT64_C(30) * 86400 * 1000000)

bool rtc_drift_time_valid(const rtc_drift_time_t *time)
{
    return time && time->fraction_q19 < RTC_DRIFT_Q19 &&
           time->us <= RTC_DRIFT_MAX_ELAPSED_US &&
           (time->us != RTC_DRIFT_MAX_ELAPSED_US || !time->fraction_q19);
}

static int time_compare(const rtc_drift_time_t *a, const rtc_drift_time_t *b)
{
    if (a->us != b->us) return a->us > b->us ? 1 : -1;
    return (a->fraction_q19 > b->fraction_q19) - (a->fraction_q19 < b->fraction_q19);
}

bool rtc_drift_time_add(const rtc_drift_time_t *a, const rtc_drift_time_t *b,
    rtc_drift_time_t *result)
{
    if (!result || !rtc_drift_time_valid(a) || !rtc_drift_time_valid(b)) return false;
    const uint32_t fraction = a->fraction_q19 + b->fraction_q19;
    if (b->us > RTC_DRIFT_MAX_ELAPSED_US - a->us) return false;
    rtc_drift_time_t value = {a->us + b->us, fraction % RTC_DRIFT_Q19};
    if (fraction >= RTC_DRIFT_Q19) ++value.us;
    if (!rtc_drift_time_valid(&value)) return false;
    *result = value;
    return true;
}

bool rtc_drift_time_sub(const rtc_drift_time_t *a, const rtc_drift_time_t *b,
    rtc_drift_time_t *result)
{
    if (!result || !rtc_drift_time_valid(a) || !rtc_drift_time_valid(b) ||
        time_compare(a, b) < 0) return false;
    rtc_drift_time_t value = {a->us - b->us, 0};
    if (a->fraction_q19 < b->fraction_q19) {
        --value.us;
        value.fraction_q19 = RTC_DRIFT_Q19 + a->fraction_q19 - b->fraction_q19;
    } else {
        value.fraction_q19 = a->fraction_q19 - b->fraction_q19;
    }
    *result = value;
    return true;
}

static bool ppm_valid(int32_t ppm)
{
    return ppm >= -RTC_DRIFT_PPM_LIMIT && ppm <= RTC_DRIFT_PPM_LIMIT;
}

bool rtc_drift_scale_time(const rtc_drift_time_t *raw, int32_t ppm,
    rtc_drift_ratio_t *ratio, rtc_drift_time_t *corrected)
{
    if (!corrected || !ratio || !rtc_drift_time_valid(raw) || !ppm_valid(ppm) ||
        (ratio->denominator ? ratio->remainder >= ratio->denominator : ratio->remainder != 0)) {
        return false;
    }
    const uint32_t denominator = (uint32_t)(1000000 + ppm);
    const uint64_t carry = ratio->denominator ?
        (uint64_t)ratio->remainder * denominator / ratio->denominator : 0;
    const uint64_t tail_us = (raw->us % denominator) * 1000000;
    const uint64_t tail = (tail_us % denominator) * RTC_DRIFT_Q19 +
                         (uint64_t)raw->fraction_q19 * 1000000 + carry;
    const uint64_t scaled_tail = tail / denominator;
    const rtc_drift_time_t value = {
        (raw->us / denominator) * 1000000 + tail_us / denominator + scaled_tail / RTC_DRIFT_Q19,
        (uint32_t)(scaled_tail % RTC_DRIFT_Q19)
    };
    if (!rtc_drift_time_valid(&value)) return false;
    *corrected = value;
    *ratio = (rtc_drift_ratio_t){denominator, (uint32_t)(tail % denominator)};
    return true;
}

static bool reference_valid(const rtc_drift_reference_t *reference)
{
    return reference && reference->continuity && rtc_drift_time_valid(&reference->raw);
}

static bool learning_valid(const rtc_drift_learning_t *state)
{
    if (!state || !state->continuity || !ppm_valid(state->ppm) ||
        (!state->qualified && state->ppm != 0)) return false;
    if (!state->baseline_valid) return !state->pending && !state->observations &&
                                      !state->last_reference_id && !state->last_seen_utc_us;
    if (!reference_valid(&state->baseline) || state->baseline.source != state->source ||
        state->baseline.continuity != state->continuity ||
        state->baseline.utc_us > state->last_seen_utc_us) return false;
    return !state->pending || (state->observations && ppm_valid(state->pending_ppm) &&
           rtc_drift_time_valid(&state->pending_exposure) &&
           state->pending_exposure.us >= RTC_DRIFT_MIN_EXPOSURE_US &&
           state->pending_uncertainty_ppm <= 200);
}

bool rtc_drift_learning_reset(rtc_drift_learning_t *state, uint32_t source,
    uint32_t continuity, int32_t saved_ppm, bool saved_qualified)
{
    if (!state || !continuity || !ppm_valid(saved_ppm)) return false;
    *state = (rtc_drift_learning_t){
        .source = source, .continuity = continuity,
        .ppm = saved_qualified ? saved_ppm : 0, .qualified = saved_qualified
    };
    return true;
}

static bool add_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    if (b > UINT64_MAX - a) return false;
    *result = a + b;
    return true;
}

static bool observation_ppm(const rtc_drift_time_t *raw, uint64_t bridge, int32_t *ppm)
{
    const rtc_drift_time_t truth = {bridge, 0};
    const bool negative = time_compare(raw, &truth) < 0;
    rtc_drift_time_t magnitude;
    if (!rtc_drift_time_sub(negative ? &truth : raw, negative ? raw : &truth, &magnitude) ||
        magnitude.us > bridge) return false;
    const uint64_t product = magnitude.us * 1000000;
    uint64_t value = product / bridge;
    const uint64_t denominator = bridge * RTC_DRIFT_Q19;
    uint64_t remainder = (product % bridge) * RTC_DRIFT_Q19 +
                         (uint64_t)magnitude.fraction_q19 * 1000000;
    value += remainder / denominator;
    remainder %= denominator;
    if (value > RTC_DRIFT_PPM_LIMIT || (value == RTC_DRIFT_PPM_LIMIT && remainder)) return false;
    if (remainder * 2 > denominator) ++value;
    *ppm = negative ? -(int32_t)value : (int32_t)value;
    return true;
}

static int weighted_compare(int32_t first, const rtc_drift_time_t *a,
    int32_t second, const rtc_drift_time_t *b, int32_t threshold)
{
    const int64_t x = 2 * first - threshold;
    const int64_t y = 2 * second - threshold;
    int64_t whole = x * (int64_t)a->us + y * (int64_t)b->us;
    int64_t fraction = x * a->fraction_q19 + y * b->fraction_q19;
    whole += fraction / RTC_DRIFT_Q19;
    fraction %= RTC_DRIFT_Q19;
    if (whole) return whole > 0 ? 1 : -1;
    return (fraction > 0) - (fraction < 0);
}

static int32_t weighted_ppm(int32_t first, const rtc_drift_time_t *a,
    int32_t second, const rtc_drift_time_t *b)
{
    int32_t low = -RTC_DRIFT_PPM_LIMIT, high = RTC_DRIFT_PPM_LIMIT;
    while (low < high) {
        const int32_t middle = low + (high - low + 1) / 2;
        if (weighted_compare(first, a, second, b, 2 * middle) >= 0) low = middle;
        else high = middle - 1;
    }
    const int comparison = weighted_compare(first, a, second, b, 2 * low + 1);
    return low + (comparison > 0 || (comparison == 0 && low < 0));
}

static rtc_drift_learn_result_t learning_rebaseline(rtc_drift_learning_t *state,
    const rtc_drift_reference_t *reference, rtc_drift_learn_result_t reason)
{
    state->baseline = *reference;
    state->last_reference_id = reference->reference_id;
    state->last_seen_utc_us = reference->utc_us;
    state->baseline_valid = true;
    if (reason != RTC_DRIFT_LEARN_BASELINE) state->pending = false;
    return reason;
}

rtc_drift_learn_result_t rtc_drift_learn(rtc_drift_learning_t *state,
    const rtc_drift_reference_t *reference)
{
    if (!learning_valid(state) || !reference_valid(reference)) return RTC_DRIFT_LEARN_INVALID;
    if (reference->source != state->source) return RTC_DRIFT_LEARN_SOURCE;
    if (reference->continuity != state->continuity) return RTC_DRIFT_LEARN_CONTINUITY;
    if (state->baseline_valid && (reference->reference_id == state->last_reference_id ||
                                  reference->utc_us <= state->last_seen_utc_us)) {
        return RTC_DRIFT_LEARN_DUPLICATE;
    }
    if (!state->baseline_valid) return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_BASELINE);
    const rtc_drift_reference_t *baseline = &state->baseline;
    const uint64_t utc = reference->utc_us - baseline->utc_us;
    const bool awake_ordered = reference->awake_us >= baseline->awake_us;
    const uint64_t awake = awake_ordered ? reference->awake_us - baseline->awake_us : 0;
    const uint64_t awake_uncertainty = awake / 20000 + (awake % 20000 != 0);
    uint64_t uncertainty;
    if (!add_u64(baseline->uncertainty_us, reference->uncertainty_us, &uncertainty) ||
        !add_u64(uncertainty, awake_uncertainty, &uncertainty)) return RTC_DRIFT_LEARN_OVERFLOW;
    const int raw_order = time_compare(&reference->raw, &baseline->raw);
    if (!awake_ordered || raw_order < 0 || utc > RTC_DRIFT_MAX_REFERENCE_US) {
        return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_INTERVAL);
    }
    if (raw_order == 0) {
        const uint64_t difference = utc >= awake ? utc - awake : awake - utc;
        if (difference > uncertainty) {
            return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_INTERVAL);
        }
        state->last_reference_id = reference->reference_id;
        state->last_seen_utc_us = reference->utc_us;
        return RTC_DRIFT_LEARN_NO_NEW_SLEEP;
    }
    if (awake >= utc) {
        return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_INTERVAL);
    }
    const uint64_t bridge = utc - awake;
    rtc_drift_time_t raw;
    if (!rtc_drift_time_sub(&reference->raw, &baseline->raw, &raw)) return RTC_DRIFT_LEARN_INVALID;
    if (raw.us < RTC_DRIFT_MIN_EXPOSURE_US) {
        state->last_reference_id = reference->reference_id;
        state->last_seen_utc_us = reference->utc_us;
        return RTC_DRIFT_LEARN_INSUFFICIENT;
    }
    int32_t ppm;
    if (!observation_ppm(&raw, bridge, &ppm)) {
        return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_RATE);
    }
    if (uncertainty > raw.us / 10000) {
        return learning_rebaseline(state, reference, RTC_DRIFT_LEARN_UNCERTAINTY);
    }
    const uint64_t uncertainty_product = uncertainty * 1000000;
    const uint32_t uncertainty_ppm = (uint32_t)(uncertainty_product / bridge +
                                               (uncertainty_product % bridge != 0));
    if (state->observations == UINT32_MAX) return RTC_DRIFT_LEARN_OVERFLOW;
    rtc_drift_learning_t next = *state;
    ++next.observations;
    rtc_drift_learn_result_t reason = RTC_DRIFT_LEARN_PENDING;
    if (state->pending) {
        const uint32_t sum = state->pending_uncertainty_ppm + uncertainty_ppm;
        const int32_t tolerance = (int32_t)(sum > 200 ? sum : 200);
        const int32_t difference = ppm - state->pending_ppm;
        if (difference >= -tolerance && difference <= tolerance) {
            next.ppm = weighted_ppm(state->pending_ppm, &state->pending_exposure, ppm, &raw);
            next.qualified = true;
            next.pending = false;
            reason = RTC_DRIFT_LEARN_PUBLISHED;
        } else {
            reason = RTC_DRIFT_LEARN_INCONSISTENT;
        }
    }
    if (reason != RTC_DRIFT_LEARN_PUBLISHED) {
        next.pending = true;
        next.pending_ppm = ppm;
        next.pending_exposure = raw;
        next.pending_uncertainty_ppm = uncertainty_ppm;
    }
    next.baseline = *reference;
    next.last_reference_id = reference->reference_id;
    next.last_seen_utc_us = reference->utc_us;
    *state = next;
    return reason;
}

/* 本文件只在普通任务运行；浮点拟合不得被 wake-stub 调用。 */
static bool temperature_valid(int32_t temp_mc)
{
    return temp_mc >= -10000 && temp_mc <= 80000;
}

static bool training_valid(const rtc_drift_training_t *training)
{
    return training && training->count <= RTC_DRIFT_OBSERVATIONS &&
           training->next < RTC_DRIFT_OBSERVATIONS &&
           (training->count == RTC_DRIFT_OBSERVATIONS || training->next == training->count);
}

void rtc_drift_training_clear(rtc_drift_training_t *training)
{
    if (training) *training = (rtc_drift_training_t){0};
}

bool rtc_drift_training_add(rtc_drift_training_t *training, int32_t temp_mc, uint32_t period_q19)
{
    if (!training_valid(training) || !temperature_valid(temp_mc) ||
        !rtc_drift_period_valid(period_q19)) return false;
    training->observations[training->next] = (rtc_drift_observation_t){temp_mc, period_q19};
    training->next = (training->next + 1) % RTC_DRIFT_OBSERVATIONS;
    if (training->count < RTC_DRIFT_OBSERVATIONS) ++training->count;
    return true;
}

/* 温区中心和平移尺度由模型边界确定，拟合变量限制在 [-1, 1]。 */
static double normalized_temperature(const rtc_drift_model_t *model, int32_t temp_mc)
{
    return (2.0 * temp_mc - model->min_temp_mc - model->max_temp_mc) /
           (model->max_temp_mc - model->min_temp_mc);
}

static double polynomial(const rtc_drift_model_t *model, double x)
{
    return model->coeff_q19[0] + x * (model->coeff_q19[1] + x * model->coeff_q19[2]);
}

static bool predicted_period_valid(double period)
{
    return isfinite(period) && period >= RTC_DRIFT_PERIOD_MIN && period <= RTC_DRIFT_PERIOD_MAX;
}

bool rtc_drift_model_validate(const rtc_drift_model_t *model)
{
    if (!model || !model->qualified ||
        !temperature_valid(model->min_temp_mc) || !temperature_valid(model->max_temp_mc) ||
        model->max_temp_mc - model->min_temp_mc < 3000 ||
        model->observations != RTC_DRIFT_OBSERVATIONS || model->occupied_bins < 3 ||
        model->occupied_bins > model->observations || !isfinite(model->rms_ppm) ||
        model->rms_ppm < 0.0 || model->rms_ppm > 100.0) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(model->coeff_q19[i])) return false;
    }
    if (!predicted_period_valid(polynomial(model, -1.0)) ||
        !predicted_period_valid(polynomial(model, 1.0))) return false;
    /* 只检查端点会漏过温区内部越界的抛物线顶点。 */
    if (model->coeff_q19[2] != 0.0) {
        const double vertex = -model->coeff_q19[1] / (2.0 * model->coeff_q19[2]);
        if (vertex > -1.0 && vertex < 1.0 &&
            !predicted_period_valid(polynomial(model, vertex))) return false;
    }
    return true;
}

bool rtc_drift_model_evaluate(const rtc_drift_model_t *model, int32_t temp_mc, double *period_q19)
{
    if (!period_q19 || !rtc_drift_model_validate(model) ||
        temp_mc < model->min_temp_mc || temp_mc > model->max_temp_mc) return false;
    const double value = polynomial(model, normalized_temperature(model, temp_mc));
    if (!predicted_period_valid(value)) return false;
    *period_q19 = value;
    return true;
}

/* 三元带部分选主元消元；矩阵位于调用任务栈，不创建工作堆。 */
static bool solve_quadratic(double matrix[3][4], double coefficients[3])
{
    for (unsigned column = 0; column < 3; ++column) {
        unsigned pivot = column;
        for (unsigned row = column + 1; row < 3; ++row) {
            if (fabs(matrix[row][column]) > fabs(matrix[pivot][column])) pivot = row;
        }
        if (fabs(matrix[pivot][column]) < 1e-10) return false;
        if (pivot != column) {
            for (unsigned i = 0; i < 4; ++i) {
                const double value = matrix[column][i];
                matrix[column][i] = matrix[pivot][i];
                matrix[pivot][i] = value;
            }
        }
        const double divisor = matrix[column][column];
        for (unsigned i = column; i < 4; ++i) matrix[column][i] /= divisor;
        for (unsigned row = 0; row < 3; ++row) {
            if (row == column) continue;
            const double factor = matrix[row][column];
            for (unsigned i = column; i < 4; ++i) matrix[row][i] -= factor * matrix[column][i];
        }
    }
    for (unsigned i = 0; i < 3; ++i) coefficients[i] = matrix[i][3];
    return true;
}

bool rtc_drift_model_fit(const rtc_drift_training_t *training, rtc_drift_model_t *model)
{
    if (!model || !training_valid(training) || training->count != RTC_DRIFT_OBSERVATIONS) return false;
    rtc_drift_model_t candidate = {.min_temp_mc = 80000, .max_temp_mc = -10000};
    unsigned bins[RTC_DRIFT_OBSERVATIONS];
    for (unsigned i = 0; i < training->count; ++i) {
        const rtc_drift_observation_t *observation = &training->observations[i];
        if (!temperature_valid(observation->temp_mc) ||
            !rtc_drift_period_valid(observation->period_q19)) return false;
        if (observation->temp_mc < candidate.min_temp_mc) candidate.min_temp_mc = observation->temp_mc;
        if (observation->temp_mc > candidate.max_temp_mc) candidate.max_temp_mc = observation->temp_mc;
        /* 先平移成非负数，负温度分箱仍相当于向下取整而非趋零。 */
        const unsigned bin = (unsigned)(observation->temp_mc + 10000) / 1000;
        unsigned existing = 0;
        while (existing < candidate.occupied_bins && bins[existing] != bin) ++existing;
        if (existing == candidate.occupied_bins) bins[candidate.occupied_bins++] = bin;
    }
    if (candidate.max_temp_mc - candidate.min_temp_mc < 3000 || candidate.occupied_bins < 3) return false;
    double matrix[3][4] = {{0}};
    for (unsigned i = 0; i < training->count; ++i) {
        const rtc_drift_observation_t *observation = &training->observations[i];
        const double x = normalized_temperature(&candidate, observation->temp_mc);
        const double terms[3] = {1.0, x, x * x};
        for (unsigned row = 0; row < 3; ++row) {
            for (unsigned column = 0; column < 3; ++column) {
                matrix[row][column] += terms[row] * terms[column];
            }
            matrix[row][3] += terms[row] * observation->period_q19;
        }
    }
    if (!solve_quadratic(matrix, candidate.coeff_q19)) return false;
    double residual = 0.0;
    for (unsigned i = 0; i < training->count; ++i) {
        const rtc_drift_observation_t *observation = &training->observations[i];
        const double predicted = polynomial(&candidate, normalized_temperature(&candidate, observation->temp_mc));
        const double ppm = (predicted - observation->period_q19) * 1000000.0 / observation->period_q19;
        residual += ppm * ppm;
    }
    candidate.rms_ppm = sqrt(residual / training->count);
    candidate.observations = training->count;
    candidate.qualified = true;
    if (!rtc_drift_model_validate(&candidate)) return false;
    /* 新观测不够合格时保留上一次有效模型；只在全部检查后替换。 */
    *model = candidate;
    return true;
}

rtc_drift_selection_t rtc_drift_select(int64_t now_us, const rtc_drift_direct_t *direct,
    const rtc_drift_temperature_t *temperature, const rtc_drift_model_t *model,
    uint32_t *period_q19)
{
    if (!period_q19 || !direct || !direct->valid || now_us < 0 || direct->stamp_us < 0 ||
        direct->stamp_us > now_us || !rtc_drift_period_valid(direct->period_q19)) {
        return RTC_DRIFT_NEEDS_MEASUREMENT;
    }
    const int64_t direct_age = now_us - direct->stamp_us;
    /* 新鲜频率测量本身不依赖温度读取成功，更不能再次叠加温补。 */
    if (direct_age <= 2000000) {
        *period_q19 = direct->period_q19;
        return RTC_DRIFT_DIRECT;
    }
    if (direct_age > 600000000 || !temperature || !temperature->valid ||
        temperature->stamp_us < 0 || temperature->stamp_us > now_us ||
        now_us - temperature->stamp_us > 1000000) return RTC_DRIFT_NEEDS_MEASUREMENT;
    double anchor_prediction, current_prediction;
    if (!rtc_drift_model_evaluate(model, direct->temp_mc, &anchor_prediction) ||
        !rtc_drift_model_evaluate(model, temperature->temp_mc, &current_prediction)) {
        return RTC_DRIFT_NEEDS_MEASUREMENT;
    }
    const double predicted = direct->period_q19 + (current_prediction - anchor_prediction);
    if (!predicted_period_valid(predicted)) return RTC_DRIFT_NEEDS_MEASUREMENT;
    *period_q19 = (uint32_t)(predicted + 0.5);
    return RTC_DRIFT_PREDICTED;
}
