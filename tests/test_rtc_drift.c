#include "rtc_drift.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: FAIL %s\n", __func__, __LINE__, #expr); \
        return false; \
    } \
} while (0)

/* 测试只使用真实数值；宿主侧允许用宽整数作为独立换算参考。 */
static bool test_fraction_and_segments(void)
{
    rtc_drift_ledger_t ledger;
    const uint32_t period = 7U * RTC_DRIFT_Q19 + 137U;
    CHECK(rtc_drift_ledger_start(&ledger, 0, period));
    CHECK(rtc_drift_ledger_advance(&ledger, 0));
    for (uint64_t i = 1; i <= 10000; ++i) {
        const uint64_t tick = i * 777;
        const uint64_t product = tick * period;
        CHECK(rtc_drift_ledger_advance(&ledger, tick));
        CHECK(ledger.elapsed_us == product / RTC_DRIFT_Q19);
        CHECK(ledger.remainder_q19 == product % RTC_DRIFT_Q19);
    }
    rtc_drift_ledger_clear(&ledger);
    CHECK(ledger.phase == RTC_DRIFT_LEDGER_EMPTY);
    CHECK(!rtc_drift_ledger_advance(&ledger, 0));
    return true;
}

static bool test_wrap_and_halfwrap(void)
{
    rtc_drift_ledger_t ledger, before;
    CHECK(rtc_drift_ledger_start(&ledger, UINT64_MAX - 2, RTC_DRIFT_PERIOD_MIN));
    CHECK(ledger.anchor_tick == RTC_DRIFT_TICK_MASK - 2);
    CHECK(rtc_drift_ledger_advance(&ledger, (UINT64_C(1) << 60) | 3));
    CHECK(ledger.elapsed_us == 12);
    before = ledger;
    CHECK(!rtc_drift_ledger_advance(&ledger, 3 + RTC_DRIFT_TICK_HALF));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(!rtc_drift_ledger_advance(&ledger, 2));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(!rtc_drift_ledger_advance(&ledger, 3 + RTC_DRIFT_TICK_HALF - 1));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    return true;
}

static bool test_period_duration_and_large_product(void)
{
    rtc_drift_ledger_t ledger, before;
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(rtc_drift_ledger_advance(&ledger, RTC_DRIFT_MAX_ELAPSED_US / 2));
    CHECK(ledger.elapsed_us == RTC_DRIFT_MAX_ELAPSED_US);
    before = ledger;
    CHECK(!rtc_drift_ledger_advance(&ledger, ledger.anchor_tick + 1));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MAX));
    CHECK(rtc_drift_ledger_advance(&ledger, RTC_DRIFT_MAX_ELAPSED_US / 20));
    CHECK(ledger.elapsed_us == RTC_DRIFT_MAX_ELAPSED_US);
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MAX));
    before = ledger;
    /* 此乘积远超 UINT64_MAX；实现必须先拆分，不能先乘再检查。 */
    CHECK(!rtc_drift_ledger_advance(&ledger, RTC_DRIFT_TICK_HALF - 1));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(!rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MIN - 1));
    CHECK(!rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MAX + 1));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MIN + 1));
    /* 分段参考值使用宿主宽整数，不与被测拆分算法共享实现。 */
    const uint64_t ticks = UINT64_C(10000000000000);
    const __uint128_t product = (__uint128_t)ticks * (RTC_DRIFT_PERIOD_MIN + 1);
    /* 合法 366 天范围的缩放值仍在 64 位内；超界乘积由前面的拒绝用例覆盖。 */
    CHECK(product <= UINT64_MAX);
    CHECK(rtc_drift_ledger_advance(&ledger, ticks));
    CHECK(ledger.elapsed_us == (uint64_t)(product >> 19));
    CHECK(ledger.remainder_q19 == (uint32_t)(product & (RTC_DRIFT_Q19 - 1)));
    return true;
}

static bool test_invalid_existing_state_is_atomic(void)
{
    rtc_drift_ledger_t valid, bad, before;
    CHECK(rtc_drift_ledger_start(&valid, 10, RTC_DRIFT_PERIOD_MIN));
    for (unsigned i = 0; i < 8; ++i) {
        bad = valid;
        switch (i) {
        case 0: bad.period_q19 = RTC_DRIFT_PERIOD_MIN - 1; break;
        case 1: bad.period_q19 = RTC_DRIFT_PERIOD_MAX + 1; break;
        case 2: bad.remainder_q19 = RTC_DRIFT_Q19; break;
        case 3: bad.elapsed_us = RTC_DRIFT_MAX_ELAPSED_US + 1; break;
        case 4: bad.anchor_tick = RTC_DRIFT_TICK_MASK + 1; break;
        case 5: bad.phase = 99; break;
        case 6: bad.phase = RTC_DRIFT_LEDGER_CONSUMED; break;
        default:
            bad.elapsed_us = RTC_DRIFT_MAX_ELAPSED_US;
            bad.remainder_q19 = 1;
            break;
        }
        before = bad;
        uint64_t elapsed = 123;
        CHECK(!rtc_drift_ledger_advance(&bad, 10));
        CHECK(!rtc_drift_ledger_commit(&bad, 10, RTC_DRIFT_PERIOD_MIN));
        CHECK(!rtc_drift_ledger_peek(&bad, 10, &elapsed));
        CHECK(!rtc_drift_ledger_consume(&bad));
        CHECK(elapsed == 123);
        CHECK(memcmp(&before, &bad, sizeof bad) == 0);
    }
    CHECK(!rtc_drift_ledger_start(NULL, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(!rtc_drift_ledger_advance(NULL, 0));
    CHECK(!rtc_drift_ledger_commit(NULL, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(!rtc_drift_ledger_peek(NULL, 0, NULL));
    CHECK(!rtc_drift_ledger_peek(&valid, 10, NULL));
    CHECK(!rtc_drift_ledger_consume(NULL));
    rtc_drift_ledger_clear(NULL);
    return true;
}

static bool test_commit_cancel_and_signed_rate_change(void)
{
    rtc_drift_ledger_t ledger, before;
    CHECK(rtc_drift_ledger_start(&ledger, 0, 2U * RTC_DRIFT_Q19 + 1));
    CHECK(rtc_drift_ledger_commit(&ledger, 1, 4U * RTC_DRIFT_Q19 + 7));
    CHECK(ledger.elapsed_us == 2 && ledger.remainder_q19 == 1);
    CHECK(rtc_drift_ledger_advance(&ledger, 2));
    CHECK(ledger.elapsed_us == 6 && ledger.remainder_q19 == 8);
    before = ledger;
    CHECK(!rtc_drift_ledger_commit(&ledger, 9, 0));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    CHECK(!rtc_drift_ledger_commit(&ledger, 1, RTC_DRIFT_PERIOD_MIN));
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    /* 提交前取消：候选周期不进入账本，整段仍按旧周期结算。 */
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(rtc_drift_ledger_advance(&ledger, 15));
    CHECK(ledger.elapsed_us == 30);
    /* 提交后取消：只有 ready_tick 后的尾段使用新周期。 */
    CHECK(rtc_drift_ledger_start(&ledger, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(rtc_drift_ledger_commit(&ledger, 10, 4U * RTC_DRIFT_Q19));
    CHECK(rtc_drift_ledger_advance(&ledger, 15));
    CHECK(ledger.elapsed_us == 40);
    CHECK(rtc_drift_ledger_commit(&ledger, 15, RTC_DRIFT_PERIOD_MIN));
    CHECK(rtc_drift_ledger_advance(&ledger, 20));
    CHECK(ledger.elapsed_us == 50);
    return true;
}

static bool test_peek_failed_apply_and_once_only_consume(void)
{
    rtc_drift_ledger_t ledger, before;
    uint64_t elapsed = 0, again = 0;
    const int64_t utc_anchor = INT64_C(1700000000000000);
    CHECK(rtc_drift_ledger_start(&ledger, 100, 3U * RTC_DRIFT_Q19));
    before = ledger;
    CHECK(rtc_drift_ledger_peek(&ledger, 110, &elapsed));
    CHECK(elapsed == 30);
    CHECK(memcmp(&before, &ledger, sizeof ledger) == 0);
    const int64_t target = utc_anchor + (int64_t)elapsed;
    /* 模拟 apply 未成功：不 consume；重试仍从同一个 UTC 锚点计算。 */
    CHECK(rtc_drift_ledger_peek(&ledger, 110, &again));
    CHECK(again == elapsed && utc_anchor + (int64_t)again == target);
    CHECK(rtc_drift_ledger_peek(&ledger, 120, &again));
    CHECK(again == 60);
    /* UTC 属于父适配器：必须在加法前检查溢出，原始账本不接收 UTC。 */
    const int64_t near_limit = INT64_MAX - 10;
    CHECK(again > (uint64_t)(INT64_MAX - near_limit));
    CHECK(rtc_drift_ledger_consume(&ledger));
    CHECK(!rtc_drift_ledger_consume(&ledger));
    CHECK(!rtc_drift_ledger_peek(&ledger, 120, &again));
    CHECK(!rtc_drift_ledger_advance(&ledger, 120));
    CHECK(!rtc_drift_ledger_commit(&ledger, 120, RTC_DRIFT_PERIOD_MIN));
    return true;
}

static bool fill_training(rtc_drift_training_t *training, double linear, double quadratic)
{
    rtc_drift_training_clear(training);
    for (int i = 0; i < RTC_DRIFT_OBSERVATIONS; ++i) {
        const double period = 4.0 * RTC_DRIFT_Q19 + linear * i + quadratic * i * i;
        if (!rtc_drift_training_add(training, 10000 + i * 1000,
                                    (uint32_t)(period + 0.5))) {
            return false;
        }
    }
    return true;
}

static bool trained_model(rtc_drift_model_t *model)
{
    rtc_drift_training_t training;
    return fill_training(&training, 200.0, 30.0) &&
           rtc_drift_model_fit(&training, model);
}

static rtc_drift_model_t constant_model(void)
{
    const rtc_drift_model_t model = {
        .coeff_q19 = {4.0 * RTC_DRIFT_Q19, 0.0, 0.0},
        .rms_ppm = 0.0,
        .min_temp_mc = -10000, .max_temp_mc = 80000,
        .observations = RTC_DRIFT_OBSERVATIONS, .occupied_bins = 12,
        .qualified = true
    };
    return model;
}

static bool test_linear_and_quadratic_qualification(void)
{
    rtc_drift_training_t training;
    rtc_drift_model_t model = {0};
    double period = 0;
    CHECK(fill_training(&training, -400.0, 0.0));
    CHECK(rtc_drift_model_fit(&training, &model));
    CHECK(rtc_drift_model_validate(&model));
    CHECK(model.rms_ppm < 0.001);
    CHECK(rtc_drift_model_evaluate(&model, 15500, &period));
    CHECK(fabs(period - (4.0 * RTC_DRIFT_Q19 - 400.0 * 5.5)) < 0.001);
    CHECK(trained_model(&model));
    CHECK(rtc_drift_model_evaluate(&model, 15500, &period));
    CHECK(fabs(period - (4.0 * RTC_DRIFT_Q19 + 200.0 * 5.5 + 30.0 * 5.5 * 5.5)) < 0.001);
    CHECK(!rtc_drift_model_evaluate(&model, 9999, &period));
    CHECK(!rtc_drift_model_evaluate(&model, 21001, &period));
    CHECK(rtc_drift_model_evaluate(&model, 10000, &period));
    CHECK(rtc_drift_model_evaluate(&model, 21000, &period));
    return true;
}

static bool test_observation_validation_and_ring(void)
{
    rtc_drift_training_t training = {0}, before;
    CHECK(sizeof training <= 112);
    CHECK(rtc_drift_training_add(&training, -10000, RTC_DRIFT_PERIOD_MIN));
    CHECK(rtc_drift_training_add(&training, 80000, RTC_DRIFT_PERIOD_MAX));
    before = training;
    CHECK(!rtc_drift_training_add(&training, -10001, RTC_DRIFT_PERIOD_MIN));
    CHECK(!rtc_drift_training_add(&training, 80001, RTC_DRIFT_PERIOD_MIN));
    CHECK(!rtc_drift_training_add(&training, 0, RTC_DRIFT_PERIOD_MIN - 1));
    CHECK(!rtc_drift_training_add(&training, 0, RTC_DRIFT_PERIOD_MAX + 1));
    CHECK(memcmp(&before, &training, sizeof training) == 0);
    CHECK(!rtc_drift_training_add(NULL, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(fill_training(&training, 0, 0));
    for (int i = 0; i < 30; ++i) {
        CHECK(rtc_drift_training_add(&training, i * 1000, 4U * RTC_DRIFT_Q19));
    }
    rtc_drift_model_t model = {0};
    CHECK(rtc_drift_model_fit(&training, &model));
    CHECK(model.min_temp_mc == 18000 && model.max_temp_mc == 29000);
    training.count = RTC_DRIFT_OBSERVATIONS + 1;
    before = training;
    CHECK(!rtc_drift_training_add(&training, 0, RTC_DRIFT_PERIOD_MIN));
    CHECK(!rtc_drift_model_fit(&training, &model));
    CHECK(memcmp(&before, &training, sizeof training) == 0);
    rtc_drift_training_clear(&training);
    training.next = RTC_DRIFT_OBSERVATIONS;
    CHECK(!rtc_drift_training_add(&training, 0, RTC_DRIFT_PERIOD_MIN));
    rtc_drift_training_clear(NULL);
    return true;
}

static bool test_spread_bins_singular_and_preservation(void)
{
    rtc_drift_training_t training = {0};
    rtc_drift_model_t model = {0}, before;
    CHECK(trained_model(&model));
    before = model;
    for (int i = 0; i < RTC_DRIFT_OBSERVATIONS - 1; ++i) {
        CHECK(rtc_drift_training_add(&training, i * 1000, 4U * RTC_DRIFT_Q19));
    }
    CHECK(!rtc_drift_model_fit(&training, &model));
    CHECK(memcmp(&before, &model, sizeof model) == 0);
    for (int mode = 0; mode < 3; ++mode) {
        rtc_drift_training_clear(&training);
        for (int i = 0; i < RTC_DRIFT_OBSERVATIONS; ++i) {
            const int temp = mode == 0 ? 0 : mode == 1 ? (i % 2) * 4000 :
                             (i % 3 == 2 ? 2999 : (i % 3) * 1000);
            CHECK(rtc_drift_training_add(&training, temp, 4U * RTC_DRIFT_Q19));
        }
        CHECK(!rtc_drift_model_fit(&training, &model));
        CHECK(memcmp(&before, &model, sizeof model) == 0);
    }
    rtc_drift_training_clear(&training);
    for (int i = 0; i < RTC_DRIFT_OBSERVATIONS; ++i) {
        /* 负温度的整数摄氏度分桶采用向下取整；边界跨度恰好 3000 mC。 */
        CHECK(rtc_drift_training_add(&training, -3000 + (i % 3) * 1500,
                                    4U * RTC_DRIFT_Q19));
    }
    CHECK(rtc_drift_model_fit(&training, &model));
    CHECK(model.occupied_bins == 3);
    CHECK(!rtc_drift_model_fit(NULL, &model));
    CHECK(!rtc_drift_model_fit(&training, NULL));
    return true;
}

static bool test_noise_outlier_and_corrupt_observation(void)
{
    rtc_drift_training_t training;
    rtc_drift_model_t model = {0}, before;
    CHECK(trained_model(&model));
    before = model;
    for (int mode = 0; mode < 3; ++mode) {
        CHECK(fill_training(&training, 200, 30));
        if (mode == 0) {
            for (int i = 0; i < RTC_DRIFT_OBSERVATIONS; ++i) {
                training.observations[i].period_q19 += (i % 2) ? 1000U : 0U;
            }
        } else if (mode == 1) {
            training.observations[5].period_q19 += 50000;
        } else {
            training.observations[5].temp_mc = 80001;
        }
        CHECK(!rtc_drift_model_fit(&training, &model));
        CHECK(memcmp(&before, &model, sizeof model) == 0);
    }
    return true;
}

static bool test_persisted_model_validation_and_vertex(void)
{
    rtc_drift_model_t valid = constant_model(), bad;
    CHECK(rtc_drift_model_validate(&valid));
    for (unsigned i = 0; i < 15; ++i) {
        bad = valid;
        switch (i) {
        case 0: bad.coeff_q19[0] = NAN; break;
        case 1: bad.coeff_q19[1] = INFINITY; break;
        case 2: bad.coeff_q19[2] = -INFINITY; break;
        case 3: bad.rms_ppm = NAN; break;
        case 4: bad.rms_ppm = INFINITY; break;
        case 5: bad.rms_ppm = -0.1; break;
        case 6: bad.rms_ppm = 100.0001; break;
        case 7: bad.min_temp_mc = -10001; break;
        case 8: bad.max_temp_mc = 80001; break;
        case 9: bad.max_temp_mc = bad.min_temp_mc; break;
        case 10: bad.max_temp_mc = bad.min_temp_mc + 2999; break;
        case 11: bad.observations = 11; break;
        case 12: bad.occupied_bins = 2; break;
        case 13: bad.occupied_bins = 13; break;
        default: bad.qualified = false; break;
        }
        double output = 123;
        CHECK(!rtc_drift_model_validate(&bad));
        CHECK(!rtc_drift_model_evaluate(&bad, 0, &output));
        CHECK(output == 123);
    }
    valid.rms_ppm = 100;
    CHECK(rtc_drift_model_validate(&valid));
    valid.coeff_q19[0] = RTC_DRIFT_PERIOD_MIN;
    CHECK(rtc_drift_model_validate(&valid));
    valid.coeff_q19[0] = RTC_DRIFT_PERIOD_MAX;
    CHECK(rtc_drift_model_validate(&valid));
    bad = valid;
    bad.coeff_q19[0] = RTC_DRIFT_PERIOD_MIN - 1.0;
    bad.coeff_q19[2] = 2;
    CHECK(!rtc_drift_model_validate(&bad));
    bad.coeff_q19[0] = RTC_DRIFT_PERIOD_MAX + 1.0;
    bad.coeff_q19[2] = -2;
    CHECK(!rtc_drift_model_validate(&bad));
    bad = constant_model();
    bad.coeff_q19[1] = RTC_DRIFT_PERIOD_MAX;
    CHECK(!rtc_drift_model_validate(&bad));
    CHECK(!rtc_drift_model_validate(NULL));
    CHECK(!rtc_drift_model_evaluate(&valid, 0, NULL));
    return true;
}

static bool test_prediction_delta_and_ledger_effect(void)
{
    rtc_drift_model_t model = {0};
    CHECK(trained_model(&model));
    rtc_drift_direct_t direct = {
        .stamp_us = 0, .temp_mc = 15000,
        .period_q19 = 4U * RTC_DRIFT_Q19 + 200U * 5 + 30U * 25 + 321,
        .valid = true
    };
    rtc_drift_temperature_t temp = {.stamp_us = 3000000, .temp_mc = 15000, .valid = true};
    uint32_t period = 0;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(period == direct.period_q19);
    temp.temp_mc = 18000;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(period == direct.period_q19 + 200U * 3 + 30U * (64 - 25));
    rtc_drift_ledger_t ledger;
    CHECK(rtc_drift_ledger_start(&ledger, 0, direct.period_q19));
    CHECK(rtc_drift_ledger_commit(&ledger, RTC_DRIFT_Q19, period));
    CHECK(ledger.elapsed_us == direct.period_q19);
    CHECK(rtc_drift_ledger_advance(&ledger, 2U * RTC_DRIFT_Q19));
    CHECK(ledger.elapsed_us == (uint64_t)direct.period_q19 + period);
    /* 睡眠无采温：已接受周期保持不变，不能事后重算旧区间。 */
    CHECK(rtc_drift_ledger_advance(&ledger, 3U * RTC_DRIFT_Q19));
    CHECK(ledger.elapsed_us == (uint64_t)direct.period_q19 + 2U * period);
    temp.temp_mc = 12000;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(period == direct.period_q19 - 200U * 3 - 30U * (25 - 4));
    /* 温度不变时差值必须恰为零，非整数模型不能把合法边界推到范围外。 */
    model = constant_model();
    model.coeff_q19[0] = RTC_DRIFT_PERIOD_MIN + 0.3;
    direct = (rtc_drift_direct_t){.stamp_us = 0, .temp_mc = 0,
        .period_q19 = RTC_DRIFT_PERIOD_MIN, .valid = true};
    temp = (rtc_drift_temperature_t){.stamp_us = 3000000, .temp_mc = 0, .valid = true};
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(period == RTC_DRIFT_PERIOD_MIN);
    return true;
}

static bool test_fresh_direct_precedence(void)
{
    rtc_drift_model_t model = {0};
    CHECK(trained_model(&model));
    rtc_drift_direct_t direct = {.stamp_us = 0, .temp_mc = 10000,
        .period_q19 = 4U * RTC_DRIFT_Q19 + 99, .valid = true};
    rtc_drift_temperature_t temp = {.stamp_us = 2000000, .temp_mc = 21000, .valid = true};
    uint32_t period = 0;
    CHECK(rtc_drift_select(0, &direct, NULL, NULL, &period) == RTC_DRIFT_DIRECT);
    CHECK(period == direct.period_q19);
    CHECK(rtc_drift_select(2000000, &direct, &temp, &model, &period) == RTC_DRIFT_DIRECT);
    CHECK(period == direct.period_q19);
    CHECK(rtc_drift_select(2000001, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(period != direct.period_q19);
    return true;
}

static bool test_age_boundaries_future_and_flags(void)
{
    rtc_drift_model_t model = constant_model();
    rtc_drift_direct_t direct = {.stamp_us = 0, .temp_mc = 0,
        .period_q19 = 4U * RTC_DRIFT_Q19, .valid = true};
    rtc_drift_temperature_t temp = {.stamp_us = 599000000, .temp_mc = 0, .valid = true};
    uint32_t period = 123;
    CHECK(rtc_drift_select(600000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(rtc_drift_select(600000001, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    temp.stamp_us = 2000000;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(rtc_drift_select(3000001, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    for (unsigned i = 0; i < 9; ++i) {
        rtc_drift_direct_t d = direct;
        rtc_drift_temperature_t t = {.stamp_us = 3000000, .temp_mc = 0, .valid = true};
        switch (i) {
        case 0: d.stamp_us = 3000001; break;
        case 1: t.stamp_us = 3000001; break;
        case 2: d.stamp_us = -1; break;
        case 3: t.stamp_us = -1; break;
        case 4: d.valid = false; break;
        case 5: t.valid = false; break;
        case 6: d.temp_mc = 80001; break;
        case 7: t.temp_mc = -10001; break;
        default: d.period_q19 = RTC_DRIFT_PERIOD_MIN - 1; break;
        }
        period = 123;
        CHECK(rtc_drift_select(3000000, &d, &t, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
        CHECK(period == 123);
    }
    temp.stamp_us = INT64_MAX;
    direct.stamp_us = INT64_MAX - 3000000;
    CHECK(rtc_drift_select(INT64_MAX, &direct, &temp, &model, &period) == RTC_DRIFT_PREDICTED);
    CHECK(rtc_drift_select(-1, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    CHECK(rtc_drift_select(0, NULL, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    CHECK(rtc_drift_select(INT64_MAX, &direct, NULL, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    CHECK(rtc_drift_select(INT64_MAX, &direct, &temp, &model, NULL) == RTC_DRIFT_NEEDS_MEASUREMENT);
    return true;
}

static bool test_prediction_range_rejects_without_false_success(void)
{
    rtc_drift_model_t model = {0};
    CHECK(trained_model(&model));
    rtc_drift_direct_t direct = {.stamp_us = 0, .temp_mc = 10000,
        .period_q19 = RTC_DRIFT_PERIOD_MAX, .valid = true};
    rtc_drift_temperature_t temp = {.stamp_us = 3000000, .temp_mc = 21000, .valid = true};
    uint32_t period = 123;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    CHECK(period == 123);
    direct.period_q19 = RTC_DRIFT_PERIOD_MIN;
    direct.temp_mc = 21000;
    temp.temp_mc = 10000;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    direct.period_q19 = 4U * RTC_DRIFT_Q19;
    temp.temp_mc = 9999;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    temp.temp_mc = 15000;
    direct.temp_mc = 21001;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    direct.temp_mc = 15000;
    CHECK(rtc_drift_select(3000000, &direct, &temp, NULL, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    model.qualified = false;
    CHECK(rtc_drift_select(3000000, &direct, &temp, &model, &period) == RTC_DRIFT_NEEDS_MEASUREMENT);
    CHECK(period == 123);
    return true;
}

static __uint128_t time_units(rtc_drift_time_t value)
{
    return (__uint128_t)value.us * RTC_DRIFT_Q19 + value.fraction_q19;
}

static bool same_time(rtc_drift_time_t a, rtc_drift_time_t b)
{
    return a.us == b.us && a.fraction_q19 == b.fraction_q19;
}

static bool test_time_arithmetic(void)
{
    rtc_drift_time_t a = {7, RTC_DRIFT_Q19 - 1}, b = {2, 2}, out = {123, 45};
    CHECK(rtc_drift_time_valid(&a));
    CHECK(!rtc_drift_time_valid(NULL));
    CHECK(rtc_drift_time_add(&a, &b, &out));
    CHECK(same_time(out, (rtc_drift_time_t){10, 1}));
    CHECK(rtc_drift_time_sub(&out, &b, &out));
    CHECK(same_time(out, a));
    CHECK(rtc_drift_time_sub(&a, &a, &out));
    CHECK(same_time(out, (rtc_drift_time_t){0}));
    rtc_drift_time_t before = out;
    CHECK(!rtc_drift_time_sub(&b, &a, &out));
    CHECK(same_time(out, before));
    const rtc_drift_time_t limit = {RTC_DRIFT_MAX_ELAPSED_US, 0};
    CHECK(rtc_drift_time_valid(&limit));
    CHECK(!rtc_drift_time_add(&limit, &b, &out));
    CHECK(same_time(out, before));
    for (unsigned i = 0; i < 3; ++i) {
        const rtc_drift_time_t bad = i == 0 ? (rtc_drift_time_t){0, RTC_DRIFT_Q19} :
            i == 1 ? (rtc_drift_time_t){RTC_DRIFT_MAX_ELAPSED_US, 1} :
                     (rtc_drift_time_t){UINT64_MAX, 0};
        CHECK(!rtc_drift_time_valid(&bad));
        CHECK(!rtc_drift_time_add(&bad, &a, &out));
        CHECK(!rtc_drift_time_sub(&bad, &a, &out));
        CHECK(same_time(out, before));
    }
    CHECK(!rtc_drift_time_add(NULL, &a, &out));
    CHECK(!rtc_drift_time_sub(&a, NULL, &out));
    CHECK(!rtc_drift_time_add(&a, &b, NULL));
    CHECK(!rtc_drift_time_sub(&a, &b, NULL));
    return true;
}

static bool test_ratio_oracle_and_segments(void)
{
    const int32_t rates[] = {-20000, -1001, -1, 0, 1, 999, 20000};
    const rtc_drift_time_t values[] = {
        {0, 0}, {0, 1}, {1, RTC_DRIFT_Q19 - 1}, {1000000, 37},
        {UINT64_C(30000000000000), 12345}, {RTC_DRIFT_MAX_ELAPSED_US, 0}
    };
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        const uint32_t denominator = (uint32_t)(1000000 + rates[i]);
        for (size_t j = 0; j < sizeof values / sizeof values[0]; ++j) {
            rtc_drift_ratio_t ratio = {0};
            rtc_drift_time_t out = {123, 45};
            const __uint128_t numerator = time_units(values[j]) * 1000000;
            const __uint128_t expected = numerator / denominator;
            const bool valid = expected <= (__uint128_t)RTC_DRIFT_MAX_ELAPSED_US * RTC_DRIFT_Q19;
            CHECK(rtc_drift_scale_time(&values[j], rates[i], &ratio, &out) == valid);
            if (valid) {
                CHECK(time_units(out) == expected);
                CHECK(ratio.denominator == denominator);
                CHECK(ratio.remainder == numerator % denominator);
            } else {
                CHECK(same_time(out, (rtc_drift_time_t){123, 45}));
                CHECK(ratio.denominator == 0 && ratio.remainder == 0);
            }
        }
        rtc_drift_ratio_t ratio = {0}, once_ratio = {0};
        rtc_drift_time_t raw_total = {0}, total = {0}, once;
        for (unsigned segment = 0; segment < 10000; ++segment) {
            const rtc_drift_time_t raw = {UINT64_C(2000000000) + segment * 37,
                                          (segment * 1777) % RTC_DRIFT_Q19};
            rtc_drift_time_t out;
            CHECK(rtc_drift_time_add(&raw_total, &raw, &raw_total));
            CHECK(rtc_drift_scale_time(&raw, rates[i], &ratio, &out));
            CHECK(rtc_drift_time_add(&total, &out, &total));
            const __uint128_t numerator = time_units(raw_total) * 1000000;
            CHECK(time_units(total) == numerator / denominator);
            CHECK(ratio.remainder == numerator % denominator);
        }
        CHECK(rtc_drift_scale_time(&raw_total, rates[i], &once_ratio, &once));
        CHECK(same_time(total, once));
        CHECK(ratio.remainder == once_ratio.remainder);
    }
    return true;
}

static bool test_ratio_changes_and_atomic_failure(void)
{
    rtc_drift_ratio_t ratio = {UINT32_MAX, UINT32_MAX - 1};
    rtc_drift_time_t out, total = {0};
    const int32_t rates[] = {20000, -20000, 1, -1, 0, 1234};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        const uint32_t denominator = (uint32_t)(1000000 + rates[i]);
        const uint64_t converted = (uint64_t)ratio.remainder * denominator / ratio.denominator;
        const rtc_drift_time_t raw = {123456789, (uint32_t)i + 1};
        const __uint128_t numerator = time_units(raw) * 1000000 + converted;
        CHECK(rtc_drift_scale_time(&raw, rates[i], &ratio, &out));
        CHECK(time_units(out) == numerator / denominator);
        CHECK(ratio.remainder == numerator % denominator);
        CHECK(rtc_drift_time_add(&total, &out, &total));
    }
    const rtc_drift_time_t raw = {10, 2};
    for (unsigned i = 0; i < 8; ++i) {
        rtc_drift_ratio_t bad_ratio = ratio;
        rtc_drift_time_t bad_raw = raw;
        int32_t ppm = 0;
        switch (i) {
        case 0: bad_ratio.denominator = 0; bad_ratio.remainder = 1; break;
        case 1: bad_ratio.remainder = bad_ratio.denominator; break;
        case 2: ppm = 20001; break;
        case 3: ppm = -20001; break;
        case 4: bad_raw.fraction_q19 = RTC_DRIFT_Q19; break;
        case 5: bad_raw.us = UINT64_MAX; break;
        case 6: bad_raw.us = RTC_DRIFT_MAX_ELAPSED_US; ppm = -1; break;
        default: bad_raw.us = RTC_DRIFT_MAX_ELAPSED_US; break;
        }
        const rtc_drift_ratio_t before = bad_ratio;
        out = (rtc_drift_time_t){123, 45};
        CHECK(!rtc_drift_scale_time(&bad_raw, ppm, &bad_ratio, &out));
        CHECK(memcmp(&before, &bad_ratio, sizeof before) == 0);
        CHECK(same_time(out, (rtc_drift_time_t){123, 45}));
    }
    CHECK(!rtc_drift_scale_time(NULL, 0, &ratio, &out));
    CHECK(!rtc_drift_scale_time(&raw, 0, NULL, &out));
    CHECK(!rtc_drift_scale_time(&raw, 0, &ratio, NULL));
    rtc_drift_time_t alias = raw;
    CHECK(rtc_drift_scale_time(&alias, 0, &ratio, &alias));
    return true;
}

#define SIX_HOURS_US (UINT64_C(6) * 3600 * 1000000)
#define THIRTY_DAYS_US (UINT64_C(30) * 86400 * 1000000)

static rtc_drift_reference_t reference_start(uint32_t source, uint32_t continuity)
{
    const rtc_drift_reference_t reference = {
        .source = source, .continuity = continuity, .reference_id = 1,
        .utc_us = UINT64_C(1700000000000000), .awake_us = 100000,
        .raw = {1000000, 7}, .uncertainty_us = 100
    };
    return reference;
}

static rtc_drift_reference_t reference_next(rtc_drift_reference_t old, uint64_t bridge,
                                           uint64_t awake, int32_t ppm)
{
    ++old.reference_id;
    old.utc_us += bridge + awake;
    old.awake_us += awake;
    old.raw.us += (uint64_t)((__int128)bridge * (1000000 + ppm) / 1000000);
    return old;
}

static rtc_drift_reference_t reference_adjust_raw(rtc_drift_reference_t ref,
                                                 int64_t us, int32_t fraction)
{
    const __int128 units = (__int128)time_units(ref.raw) +
                           (__int128)us * RTC_DRIFT_Q19 + fraction;
    ref.raw = (rtc_drift_time_t){(uint64_t)(units / RTC_DRIFT_Q19),
                                (uint32_t)(units % RTC_DRIFT_Q19)};
    return ref;
}

static bool test_learning_opaque_ids(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 0, false));
    rtc_drift_reference_t ref = reference_start(2, 42);
    ref.reference_id = (UINT64_C(0xf0000000) << 32) | 1;
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US / 6, 0, 0);
    ref.reference_id = (UINT64_C(0x10000000) << 32) | 1;
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INSUFFICIENT);
    CHECK(state.last_reference_id == ref.reference_id);
    ref = reference_next(ref, SIX_HOURS_US, 0, 0);
    ref.reference_id = 0;
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    CHECK(state.last_reference_id == 0 && state.pending_ppm == 0);
    ref = reference_next(ref, SIX_HOURS_US, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
    CHECK(state.ppm == 0 && state.observations == 2);
    return true;
}

static bool test_learning_chronology_preserves_pending(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 0, false));
    rtc_drift_reference_t ref = reference_start(2, 42);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US * 2, 0, 1000);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    const rtc_drift_reference_t baseline = ref;
    ref = reference_next(ref, SIX_HOURS_US / 6, 0, 1000);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INSUFFICIENT);
    const rtc_drift_learning_t before = state;
    for (unsigned i = 0; i < 3; ++i) {
        rtc_drift_reference_t replay = reference_next(ref, SIX_HOURS_US * 2, 0, 1000);
        if (i == 0) replay.utc_us = baseline.utc_us + SIX_HOURS_US / 12;
        if (i == 1) replay.utc_us = ref.utc_us;
        if (i == 2) replay.reference_id = ref.reference_id;
        replay.raw = baseline.raw;
        CHECK(rtc_drift_learn(&state, &replay) == RTC_DRIFT_LEARN_DUPLICATE);
        CHECK(memcmp(&state, &before, sizeof state) == 0);
    }
    ref = reference_next(ref, SIX_HOURS_US, 0, 1000);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
    CHECK(state.ppm == 1000 && state.observations == 2);
    return true;
}

static bool check_learning_exact_rate_limit(int sign, unsigned edge)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 321, true));
    rtc_drift_reference_t ref = reference_start(2, 42);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US * 4, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    ref = reference_next(ref, SIX_HOURS_US * 4, 0, sign * 20000);
    if (edge == 2) ++ref.utc_us;
    ref = reference_adjust_raw(ref, edge == 1 ? sign * INT64_C(34560) : edge == 2 ? 1 : 0,
                               edge == 1 ? 0 : edge == 2 ?
                               sign * (int32_t)(RTC_DRIFT_Q19 / 50 + 1) : sign);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_RATE);
    CHECK(!state.pending && state.observations == 1);
    CHECK(state.qualified && state.ppm == 321);
    CHECK(state.baseline.utc_us == ref.utc_us && state.last_reference_id == ref.reference_id);
    return true;
}

static bool test_learning_positive_fractional_rate_limit(void)
{
    return check_learning_exact_rate_limit(1, true);
}

static bool test_learning_negative_fractional_rate_limit(void)
{
    return check_learning_exact_rate_limit(-1, true);
}

static bool test_learning_positive_q19_rate_limit(void)
{
    return check_learning_exact_rate_limit(1, false);
}

static bool test_learning_negative_q19_rate_limit(void)
{
    return check_learning_exact_rate_limit(-1, false);
}

static bool test_learning_positive_submicrosecond_rate_limit(void)
{
    return check_learning_exact_rate_limit(1, 2);
}

static bool test_learning_negative_submicrosecond_rate_limit(void)
{
    return check_learning_exact_rate_limit(-1, 2);
}

static bool test_learning_integer_candidate_weighting(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 0, false));
    rtc_drift_reference_t ref = reference_start(2, 42);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_adjust_raw(reference_next(ref, SIX_HOURS_US * 4, 0, 1000), 34560, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    CHECK(state.pending_ppm == 1000);
    rtc_drift_learning_t second = state;
    second.pending = false;
    ref = reference_adjust_raw(reference_next(ref, SIX_HOURS_US * 2, 0, 1000), 34560, 0);
    CHECK(rtc_drift_learn(&second, &ref) == RTC_DRIFT_LEARN_PENDING);
    CHECK(second.pending_ppm == 1001);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
    CHECK(state.qualified && state.ppm == 1000 && state.observations == 2);
    return true;
}

static bool test_learning_invalid_pending_exposure(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 0, false));
    rtc_drift_reference_t ref = reference_start(2, 42);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    state.pending_exposure = (rtc_drift_time_t){RTC_DRIFT_MAX_ELAPSED_US, 1};
    const rtc_drift_learning_t before = state;
    ref = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INVALID);
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    return true;
}

static bool test_learning_watermark_validity_and_reset(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 2, 42, 0, false));
    CHECK(state.last_seen_utc_us == 0 && state.last_reference_id == 0);
    rtc_drift_reference_t ref = reference_start(2, 42);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    CHECK(state.last_seen_utc_us == ref.utc_us && state.last_reference_id == ref.reference_id);
    ref = reference_next(ref, SIX_HOURS_US / 6, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INSUFFICIENT);
    CHECK(state.last_seen_utc_us == ref.utc_us && state.last_reference_id == ref.reference_id);
    CHECK(state.baseline.utc_us < state.last_seen_utc_us);
    ref = reference_next(ref, SIX_HOURS_US, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    CHECK(state.last_seen_utc_us == ref.utc_us && state.last_reference_id == ref.reference_id);
    rtc_drift_learning_t bad = state;
    --bad.last_seen_utc_us;
    rtc_drift_learning_t before = bad;
    rtc_drift_reference_t next = reference_next(ref, SIX_HOURS_US, 0, 0);
    CHECK(rtc_drift_learn(&bad, &next) == RTC_DRIFT_LEARN_INVALID);
    CHECK(memcmp(&bad, &before, sizeof bad) == 0);
    bad = state;
    bad.observations = UINT32_MAX;
    before = bad;
    CHECK(rtc_drift_learn(&bad, &next) == RTC_DRIFT_LEARN_OVERFLOW);
    CHECK(memcmp(&bad, &before, sizeof bad) == 0);
    CHECK(rtc_drift_learn(&state, &next) == RTC_DRIFT_LEARN_PUBLISHED);
    CHECK(state.last_seen_utc_us == next.utc_us && state.last_reference_id == next.reference_id);
    ref = reference_next(next, SIX_HOURS_US, 0, 3000);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    ref = reference_next(ref, SIX_HOURS_US, 0, 4000);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INCONSISTENT);
    CHECK(state.last_seen_utc_us == ref.utc_us && state.last_reference_id == ref.reference_id);
    CHECK(rtc_drift_learning_reset(&state, 2, 43, 123, true));
    CHECK(state.last_seen_utc_us == 0 && state.last_reference_id == 0);
    CHECK(state.qualified && state.ppm == 123 && !state.baseline_valid && !state.pending);
    ref = reference_start(2, 43);
    bad = state;
    bad.last_seen_utc_us = 1;
    before = bad;
    CHECK(rtc_drift_learn(&bad, &ref) == RTC_DRIFT_LEARN_INVALID);
    CHECK(memcmp(&bad, &before, sizeof bad) == 0);
    ref.utc_us = 0;
    ref.reference_id = 0;
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    CHECK(state.last_seen_utc_us == ref.utc_us);
    return true;
}

static bool test_learning_two_windows_and_raw_history(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        const int32_t ppm = mode == 0 ? 1200 : mode == 1 ? -1700 : 0;
        rtc_drift_learning_t state;
        CHECK(rtc_drift_learning_reset(&state, 2, 42, 17777, true));
        rtc_drift_reference_t ref = reference_start(2, 42);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        ref = reference_next(ref, SIX_HOURS_US * 2, 8000000, ppm);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        CHECK(state.observations == 1 && state.ppm == 17777 && state.qualified);
        CHECK(state.pending_ppm == ppm);
        const rtc_drift_learning_t before = state;
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_DUPLICATE);
        CHECK(memcmp(&state, &before, sizeof state) == 0);
        ref = reference_next(ref, SIX_HOURS_US * 2, 1000000, ppm);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
        CHECK(state.qualified && state.ppm == ppm && state.observations == 2);
        CHECK(!state.pending);
    }
    return true;
}

static bool test_learning_insufficient_and_nonoverlap(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, 0, 1, 0, false));
    rtc_drift_reference_t ref = reference_start(0, 1);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    const rtc_drift_reference_t baseline = state.baseline;
    for (unsigned hour = 1; hour <= 5; ++hour) {
        ref = reference_next(ref, SIX_HOURS_US / 6, 200000, 0);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INSUFFICIENT);
        CHECK(state.baseline.reference_id == baseline.reference_id);
        CHECK(state.observations == 0 && !state.qualified);
    }
    const rtc_drift_learning_t before = state;
    rtc_drift_reference_t replay = baseline;
    CHECK(rtc_drift_learn(&state, &replay) == RTC_DRIFT_LEARN_DUPLICATE);
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    ref = reference_next(ref, SIX_HOURS_US / 6, 200000, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    ref = reference_next(ref, SIX_HOURS_US / 6, 200000, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INSUFFICIENT);
    CHECK(state.observations == 1 && !state.qualified);
    ref = reference_next(ref, SIX_HOURS_US, 200000, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
    CHECK(state.observations == 2 && state.ppm == 0);
    return true;
}

static bool test_learning_weighting_rounding_and_consistency(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        rtc_drift_learning_t state;
        CHECK(rtc_drift_learning_reset(&state, 91, 3, 0, false));
        rtc_drift_reference_t ref = reference_start(91, 3);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 1000 * sign);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        ref = reference_next(ref, SIX_HOURS_US * 4, 0, 1100 * sign);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
        CHECK(state.ppm == 1067 * sign);
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 3000 * sign);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 4000 * sign);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_INCONSISTENT);
        CHECK(state.ppm == 1067 * sign && state.qualified && state.observations == 4);
        CHECK(state.pending_ppm == 4000 * sign);
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 4200 * sign);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PUBLISHED);
        CHECK(state.ppm == 4100 * sign);
        CHECK(rtc_drift_learning_reset(&state, 91, 3, 0, false));
        ref = reference_start(91, 3);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        for (unsigned i = 0; i < 2; ++i) {
            rtc_drift_reference_t next = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
            const __uint128_t delta = (__uint128_t)(SIX_HOURS_US * 2) *
                                     (2000000 + sign * 2001) * RTC_DRIFT_Q19 / 2000000;
            const __uint128_t units = time_units(ref.raw) + delta;
            next.raw = (rtc_drift_time_t){(uint64_t)(units / RTC_DRIFT_Q19),
                                           (uint32_t)(units % RTC_DRIFT_Q19)};
            CHECK(rtc_drift_learn(&state, &next) ==
                  (i == 0 ? RTC_DRIFT_LEARN_PENDING : RTC_DRIFT_LEARN_PUBLISHED));
            ref = next;
        }
        CHECK(state.ppm == 1000 * sign);
    }
    return true;
}

static bool test_learning_rejections_and_rebaseline(void)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        rtc_drift_learning_t state;
        CHECK(rtc_drift_learning_reset(&state, 0, 1, 321, true));
        rtc_drift_reference_t ref = reference_start(0, 1);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
        rtc_drift_learn_result_t reason;
        switch (mode) {
        case 0: ref.utc_us = state.baseline.utc_us; reason = RTC_DRIFT_LEARN_DUPLICATE; break;
        case 1: ref.awake_us = state.baseline.awake_us - 1; reason = RTC_DRIFT_LEARN_INTERVAL; break;
        case 2: ref.awake_us += SIX_HOURS_US * 2; reason = RTC_DRIFT_LEARN_INTERVAL; break;
        case 3: ref.utc_us += THIRTY_DAYS_US; reason = RTC_DRIFT_LEARN_INTERVAL; break;
        case 4: ref.raw.us += SIX_HOURS_US; reason = RTC_DRIFT_LEARN_RATE; break;
        default: ref.uncertainty_us = SIX_HOURS_US; reason = RTC_DRIFT_LEARN_UNCERTAINTY; break;
        }
        const rtc_drift_learning_t before = state;
        CHECK(rtc_drift_learn(&state, &ref) == reason);
        if (mode == 0) CHECK(memcmp(&state, &before, sizeof state) == 0);
        else {
            CHECK(state.baseline.reference_id == ref.reference_id);
            CHECK(state.last_reference_id == ref.reference_id && state.last_seen_utc_us == ref.utc_us);
        }
        CHECK(state.qualified && state.ppm == 321 && state.observations == 0);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_DUPLICATE);
        if (mode == 0) ref = state.baseline;
        ref.uncertainty_us = 100;
        ref = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
        if (mode == 5) {
            CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_UNCERTAINTY);
            ref = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
        }
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    }
    return true;
}

static bool test_learning_uncertainty_and_boundaries(void)
{
    rtc_drift_learning_t state;
    rtc_drift_reference_t ref = reference_start(0, 1);
    ref.uncertainty_us = SIX_HOURS_US / 20000;
    CHECK(rtc_drift_learning_reset(&state, 0, 1, 0, false));
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US, 0, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    ref = reference_next(ref, SIX_HOURS_US, 1, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_UNCERTAINTY);
    CHECK(state.observations == 1 && !state.qualified);
    for (unsigned i = 0; i < 4; ++i) {
        const int32_t ppm = i % 2 ? -20000 : 20000;
        CHECK(rtc_drift_learning_reset(&state, 2, 8, 0, false));
        ref = reference_start(2, 8);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        ref = reference_next(ref, i < 2 ? SIX_HOURS_US * 2 : THIRTY_DAYS_US, 0, ppm);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        CHECK(state.pending_ppm == ppm);
    }
    return true;
}

static bool test_learning_reset_source_invalid_and_overflow(void)
{
    rtc_drift_learning_t state;
    CHECK(rtc_drift_learning_reset(&state, UINT32_MAX, 4, 0, false));
    CHECK(!state.qualified && state.ppm == 0 && state.observations == 0);
    rtc_drift_reference_t ref = reference_start(UINT32_MAX, 4);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    const rtc_drift_learning_t before = state;
    CHECK(!rtc_drift_learning_reset(&state, 0, 0, 0, false));
    CHECK(!rtc_drift_learning_reset(&state, 0, 1, 20001, true));
    CHECK(!rtc_drift_learning_reset(NULL, 0, 1, 0, false));
    CHECK(memcmp(&state, &before, sizeof state) == 0);
    for (unsigned i = 0; i < 6; ++i) {
        rtc_drift_reference_t bad = reference_next(ref, SIX_HOURS_US * 2, 0, 0);
        rtc_drift_learn_result_t reason = RTC_DRIFT_LEARN_INVALID;
        switch (i) {
        case 0: bad.source = 0; reason = RTC_DRIFT_LEARN_SOURCE; break;
        case 1: ++bad.continuity; reason = RTC_DRIFT_LEARN_CONTINUITY; break;
        case 2: bad.continuity = 0; break;
        case 3: bad.raw.fraction_q19 = RTC_DRIFT_Q19; break;
        case 4: bad.raw.us = UINT64_MAX; break;
        default: bad.uncertainty_us = UINT64_MAX; reason = RTC_DRIFT_LEARN_OVERFLOW; break;
        }
        CHECK(rtc_drift_learn(&state, &bad) == reason);
        CHECK(memcmp(&state, &before, sizeof state) == 0);
    }
    CHECK(rtc_drift_learn(NULL, &ref) == RTC_DRIFT_LEARN_INVALID);
    CHECK(rtc_drift_learn(&state, NULL) == RTC_DRIFT_LEARN_INVALID);
    CHECK(rtc_drift_learning_reset(&state, UINT32_MAX, 5, 321, true));
    CHECK(!state.baseline_valid && !state.pending && state.observations == 0);
    CHECK(state.ppm == 321 && state.qualified);
    CHECK(rtc_drift_learning_reset(&state, 2, 6, 0, false));
    CHECK(state.ppm == 0 && !state.qualified);
    ref = reference_start(2, 6);
    ref.utc_us = UINT64_MAX - SIX_HOURS_US * 4;
    ref.awake_us = UINT64_MAX - 100;
    ref.reference_id = 0;
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
    ref = reference_next(ref, SIX_HOURS_US * 2, 50, 0);
    CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
    rtc_drift_learning_t bad = state;
    bad.pending_ppm = INT32_MAX;
    rtc_drift_learning_t bad_before = bad;
    CHECK(rtc_drift_learn(&bad, &ref) == RTC_DRIFT_LEARN_INVALID);
    CHECK(memcmp(&bad, &bad_before, sizeof bad) == 0);
    return true;
}

static bool test_learning_awake_only_preservation(void)
{
    for (unsigned pending = 0; pending < 2; ++pending) {
        rtc_drift_learning_t state;
        CHECK(rtc_drift_learning_reset(&state, 2, 42, -870, true));
        rtc_drift_reference_t ref = reference_start(2, 42);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        if (pending) {
            ref = reference_next(ref, SIX_HOURS_US, 0, 1000);
            CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        }
        rtc_drift_learning_t expected = state;
        for (unsigned i = 0; i < 4; ++i) {
            ref = reference_next(ref, 0, 1000000, 0);
            ref.uncertainty_us += 17;
            const rtc_drift_learn_result_t result = rtc_drift_learn(&state, &ref);
            CHECK(result == RTC_DRIFT_LEARN_NO_NEW_SLEEP);
            expected.last_reference_id = ref.reference_id;
            expected.last_seen_utc_us = ref.utc_us;
            CHECK(memcmp(&state, &expected, sizeof state) == 0);
            CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_DUPLICATE);
        }
        ref = reference_next(ref, SIX_HOURS_US, 0, 1000);
        CHECK(rtc_drift_learn(&state, &ref) ==
              (pending ? RTC_DRIFT_LEARN_PUBLISHED : RTC_DRIFT_LEARN_PENDING));
        CHECK(state.observations == pending + 1);
        CHECK(state.ppm == (pending ? 1000 : -870));
    }
    return true;
}

static bool test_learning_awake_only_validation(void)
{
    for (unsigned mode = 0; mode < 15; ++mode) {
        rtc_drift_learning_t state;
        CHECK(rtc_drift_learning_reset(&state, 2, 42, -870, true));
        rtc_drift_reference_t ref = reference_start(2, 42);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_BASELINE);
        ref = reference_next(ref, SIX_HOURS_US, 0, 1000);
        CHECK(rtc_drift_learn(&state, &ref) == RTC_DRIFT_LEARN_PENDING);
        const rtc_drift_learning_t before = state;
        ref = reference_next(ref, 0, 20000, 0);
        rtc_drift_learn_result_t reason = RTC_DRIFT_LEARN_INTERVAL;
        bool atomic = false;
        switch (mode) {
        case 0: ref.utc_us += 201; reason = RTC_DRIFT_LEARN_NO_NEW_SLEEP; break;
        case 1: ref.utc_us -= 201; reason = RTC_DRIFT_LEARN_NO_NEW_SLEEP; break;
        case 2: ref.utc_us += 202; break;
        case 3: ref.utc_us -= 202; break;
        case 4: ref.awake_us = state.baseline.awake_us - 1; break;
        case 5: --ref.raw.fraction_q19; break;
        case 6: ref.source = 0; reason = RTC_DRIFT_LEARN_SOURCE; atomic = true; break;
        case 7: ++ref.continuity; reason = RTC_DRIFT_LEARN_CONTINUITY; atomic = true; break;
        case 8: ref.reference_id = state.last_reference_id; reason = RTC_DRIFT_LEARN_DUPLICATE; atomic = true; break;
        case 9: ref.utc_us = state.last_seen_utc_us - 1; reason = RTC_DRIFT_LEARN_DUPLICATE; atomic = true; break;
        case 10: ref.raw.fraction_q19 = RTC_DRIFT_Q19; reason = RTC_DRIFT_LEARN_INVALID; atomic = true; break;
        case 11: ref.uncertainty_us = UINT64_MAX; reason = RTC_DRIFT_LEARN_OVERFLOW; atomic = true; break;
        case 12: ref.uncertainty_us = UINT64_MAX - 100; reason = RTC_DRIFT_LEARN_OVERFLOW; atomic = true; break;
        case 13: ref = reference_next(state.baseline, 0, THIRTY_DAYS_US + 1, 0); break;
        default: ++ref.raw.fraction_q19; break;
        }
        CHECK(rtc_drift_learn(&state, &ref) == reason);
        if (atomic) CHECK(memcmp(&state, &before, sizeof state) == 0);
        else if (mode < 2) {
            rtc_drift_learning_t expected = before;
            expected.last_reference_id = ref.reference_id;
            expected.last_seen_utc_us = ref.utc_us;
            CHECK(memcmp(&state, &expected, sizeof state) == 0);
        } else {
            CHECK(!state.pending && state.ppm == -870 && state.qualified);
            CHECK(state.baseline.utc_us == ref.utc_us);
        }
    }
    return true;
}

int main(void)
{
    static const struct { const char *name; bool (*run)(void); } tests[] = {
        {"learning_awake_only_preservation", test_learning_awake_only_preservation},
        {"learning_awake_only_validation", test_learning_awake_only_validation},
        {"time_arithmetic", test_time_arithmetic},
        {"ratio_oracle_and_segments", test_ratio_oracle_and_segments},
        {"ratio_changes_and_atomic_failure", test_ratio_changes_and_atomic_failure},
        {"learning_watermark_validity_and_reset", test_learning_watermark_validity_and_reset},
        {"learning_opaque_ids", test_learning_opaque_ids},
        {"learning_chronology_preserves_pending", test_learning_chronology_preserves_pending},
        {"learning_positive_fractional_rate_limit", test_learning_positive_fractional_rate_limit},
        {"learning_negative_fractional_rate_limit", test_learning_negative_fractional_rate_limit},
        {"learning_positive_q19_rate_limit", test_learning_positive_q19_rate_limit},
        {"learning_negative_q19_rate_limit", test_learning_negative_q19_rate_limit},
        {"learning_positive_submicrosecond_rate_limit", test_learning_positive_submicrosecond_rate_limit},
        {"learning_negative_submicrosecond_rate_limit", test_learning_negative_submicrosecond_rate_limit},
        {"learning_integer_candidate_weighting", test_learning_integer_candidate_weighting},
        {"learning_invalid_pending_exposure", test_learning_invalid_pending_exposure},
        {"learning_two_windows_and_raw_history", test_learning_two_windows_and_raw_history},
        {"learning_insufficient_and_nonoverlap", test_learning_insufficient_and_nonoverlap},
        {"learning_weighting_rounding_and_consistency", test_learning_weighting_rounding_and_consistency},
        {"learning_rejections_and_rebaseline", test_learning_rejections_and_rebaseline},
        {"learning_uncertainty_and_boundaries", test_learning_uncertainty_and_boundaries},
        {"learning_reset_source_invalid_and_overflow", test_learning_reset_source_invalid_and_overflow},
        {"fraction_and_segments", test_fraction_and_segments},
        {"wrap_and_halfwrap", test_wrap_and_halfwrap},
        {"period_duration_and_large_product", test_period_duration_and_large_product},
        {"invalid_existing_state_is_atomic", test_invalid_existing_state_is_atomic},
        {"commit_cancel_and_signed_rate_change", test_commit_cancel_and_signed_rate_change},
        {"peek_failed_apply_and_once_only_consume", test_peek_failed_apply_and_once_only_consume},
        {"linear_and_quadratic_qualification", test_linear_and_quadratic_qualification},
        {"observation_validation_and_ring", test_observation_validation_and_ring},
        {"spread_bins_singular_and_preservation", test_spread_bins_singular_and_preservation},
        {"noise_outlier_and_corrupt_observation", test_noise_outlier_and_corrupt_observation},
        {"persisted_model_validation_and_vertex", test_persisted_model_validation_and_vertex},
        {"prediction_delta_and_ledger_effect", test_prediction_delta_and_ledger_effect},
        {"fresh_direct_precedence", test_fresh_direct_precedence},
        {"age_boundaries_future_and_flags", test_age_boundaries_future_and_flags},
        {"prediction_range_rejects_without_false_success", test_prediction_range_rejects_without_false_success}
    };
    unsigned passed = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
        const bool ok = tests[i].run();
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        passed += ok;
    }
    printf("%u/%zu passed; sizes ledger=%zu training=%zu model=%zu direct=%zu temperature=%zu\n",
           passed, sizeof tests / sizeof tests[0], sizeof(rtc_drift_ledger_t),
           sizeof(rtc_drift_training_t), sizeof(rtc_drift_model_t),
           sizeof(rtc_drift_direct_t), sizeof(rtc_drift_temperature_t));
    return passed == sizeof tests / sizeof tests[0] ? 0 : 1;
}
