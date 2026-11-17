#ifndef RTC_DRIFT_H
#define RTC_DRIFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_DRIFT_Q19 (UINT32_C(1) << 19)
#define RTC_DRIFT_PERIOD_MIN (UINT32_C(2) * RTC_DRIFT_Q19)
#define RTC_DRIFT_PERIOD_MAX (UINT32_C(20) * RTC_DRIFT_Q19)
#define RTC_DRIFT_TICK_MASK ((UINT64_C(1) << 48) - 1)
#define RTC_DRIFT_TICK_HALF (UINT64_C(1) << 47)
#define RTC_DRIFT_MAX_ELAPSED_US (UINT64_C(366) * 86400 * 1000000)
#define RTC_DRIFT_OBSERVATIONS 12

#if defined(__GNUC__) || defined(__clang__)
#define RTC_DRIFT_INLINE static inline __attribute__((always_inline))
#else
#error "rtc_drift requires an always_inline capable compiler"
#endif

enum {
    RTC_DRIFT_LEDGER_EMPTY = 0,
    RTC_DRIFT_LEDGER_ACTIVE = 1,
    RTC_DRIFT_LEDGER_CONSUMED = 2
};

typedef struct {
    uint64_t anchor_tick;
    uint64_t elapsed_us;
    uint32_t period_q19;
    uint32_t remainder_q19;
    uint32_t phase;
} rtc_drift_ledger_t;

/* 这些整数函数会内联到 RTC 单元；不能引入浮点、分配器或结构体复制。 */
RTC_DRIFT_INLINE bool rtc_drift_period_valid(uint32_t period)
{
    return period >= RTC_DRIFT_PERIOD_MIN && period <= RTC_DRIFT_PERIOD_MAX;
}

RTC_DRIFT_INLINE bool rtc_drift_ledger_valid(const rtc_drift_ledger_t *ledger)
{
    return ledger && ledger->phase == RTC_DRIFT_LEDGER_ACTIVE &&
           ledger->anchor_tick <= RTC_DRIFT_TICK_MASK &&
           rtc_drift_period_valid(ledger->period_q19) &&
           ledger->remainder_q19 < RTC_DRIFT_Q19 &&
           ledger->elapsed_us <= RTC_DRIFT_MAX_ELAPSED_US &&
           (ledger->elapsed_us != RTC_DRIFT_MAX_ELAPSED_US || !ledger->remainder_q19);
}

RTC_DRIFT_INLINE void rtc_drift_ledger_clear(rtc_drift_ledger_t *ledger)
{
    if (!ledger) return;
    ledger->anchor_tick = 0;
    ledger->elapsed_us = 0;
    ledger->period_q19 = 0;
    ledger->remainder_q19 = 0;
    ledger->phase = RTC_DRIFT_LEDGER_EMPTY;
}

RTC_DRIFT_INLINE bool rtc_drift_ledger_start(rtc_drift_ledger_t *ledger, uint64_t tick, uint32_t period)
{
    if (!ledger || !rtc_drift_period_valid(period)) return false;
    ledger->anchor_tick = tick & RTC_DRIFT_TICK_MASK;
    ledger->elapsed_us = 0;
    ledger->period_q19 = period;
    ledger->remainder_q19 = 0;
    ledger->phase = RTC_DRIFT_LEDGER_ACTIVE;
    return true;
}

/* 先在局部值计算并验证，失败时不发布任何检查点或输出值。 */
RTC_DRIFT_INLINE bool rtc_drift_ledger_value_at(const rtc_drift_ledger_t *ledger,
                                             uint64_t tick, uint64_t *elapsed,
                                             uint32_t *remainder)
{
    if (!rtc_drift_ledger_valid(ledger)) return false;
    const uint64_t delta = ((tick & RTC_DRIFT_TICK_MASK) - ledger->anchor_tick) & RTC_DRIFT_TICK_MASK;
    if (delta >= RTC_DRIFT_TICK_HALF) return false;
    const uint64_t whole = delta >> 19;
    const uint64_t low = delta & (RTC_DRIFT_Q19 - 1);
    const uint64_t tail = low * ledger->period_q19 + ledger->remainder_q19;
    const uint64_t advance = whole * ledger->period_q19 + (tail >> 19);
    const uint32_t fraction = (uint32_t)(tail & (RTC_DRIFT_Q19 - 1));
    if (advance > RTC_DRIFT_MAX_ELAPSED_US - ledger->elapsed_us) return false;
    const uint64_t total = ledger->elapsed_us + advance;
    if (total == RTC_DRIFT_MAX_ELAPSED_US && fraction) return false;
    *elapsed = total;
    *remainder = fraction;
    return true;
}

RTC_DRIFT_INLINE bool rtc_drift_ledger_advance(rtc_drift_ledger_t *ledger, uint64_t tick)
{
    uint64_t elapsed;
    uint32_t remainder;
    if (!rtc_drift_ledger_value_at(ledger, tick, &elapsed, &remainder)) return false;
    ledger->anchor_tick = tick & RTC_DRIFT_TICK_MASK;
    ledger->elapsed_us = elapsed;
    ledger->remainder_q19 = remainder;
    return true;
}

/* ready tick 前用旧周期；新周期只对已提交检查点之后的时间生效。 */
RTC_DRIFT_INLINE bool rtc_drift_ledger_commit(rtc_drift_ledger_t *ledger, uint64_t tick, uint32_t period)
{
    if (!rtc_drift_period_valid(period) || !rtc_drift_ledger_advance(ledger, tick)) return false;
    ledger->period_q19 = period;
    return true;
}

/* 设置 UTC 失败时可重试读取；不提前消费或推进保留检查点。 */
RTC_DRIFT_INLINE bool rtc_drift_ledger_peek(const rtc_drift_ledger_t *ledger, uint64_t tick, uint64_t *elapsed)
{
    uint64_t value;
    uint32_t remainder;
    if (!elapsed || !rtc_drift_ledger_value_at(ledger, tick, &value, &remainder)) return false;
    *elapsed = value;
    return true;
}

RTC_DRIFT_INLINE bool rtc_drift_ledger_consume(rtc_drift_ledger_t *ledger)
{
    if (!rtc_drift_ledger_valid(ledger)) return false;
    ledger->phase = RTC_DRIFT_LEDGER_CONSUMED;
    return true;
}

typedef struct {
    int32_t temp_mc;
    uint32_t period_q19;
} rtc_drift_observation_t;

typedef struct {
    rtc_drift_observation_t observations[RTC_DRIFT_OBSERVATIONS];
    uint32_t count;
    uint32_t next;
} rtc_drift_training_t;

/* x=(2*T-Tmin-Tmax)/(Tmax-Tmin)，系数表示归一化温度下的 Q19 周期。 */
typedef struct {
    double coeff_q19[3];
    double rms_ppm;
    int32_t min_temp_mc;
    int32_t max_temp_mc;
    uint32_t observations;
    uint32_t occupied_bins;
    bool qualified;
} rtc_drift_model_t;

typedef struct {
    int64_t stamp_us;
    int32_t temp_mc;
    uint32_t period_q19;
    bool valid;
} rtc_drift_direct_t;

typedef struct {
    int64_t stamp_us;
    int32_t temp_mc;
    bool valid;
} rtc_drift_temperature_t;

typedef enum {
    RTC_DRIFT_NEEDS_MEASUREMENT = 0,
    RTC_DRIFT_DIRECT = 1,
    RTC_DRIFT_PREDICTED = 2
} rtc_drift_selection_t;

typedef struct {
    uint64_t us;
    uint32_t fraction_q19;
} rtc_drift_time_t;

typedef struct {
    uint32_t denominator;
    uint32_t remainder;
} rtc_drift_ratio_t;

typedef struct {
    uint32_t source;
    uint32_t continuity;
    uint64_t reference_id;
    uint64_t utc_us;
    uint64_t awake_us;
    rtc_drift_time_t raw;
    uint64_t uncertainty_us;
} rtc_drift_reference_t;

typedef struct {
    uint32_t source;
    uint32_t continuity;
    rtc_drift_reference_t baseline;
    uint64_t last_reference_id;
    uint64_t last_seen_utc_us;
    rtc_drift_time_t pending_exposure;
    int32_t pending_ppm;
    uint32_t pending_uncertainty_ppm;
    int32_t ppm;
    uint32_t observations;
    bool baseline_valid;
    bool pending;
    bool qualified;
} rtc_drift_learning_t;

typedef enum {
    RTC_DRIFT_LEARN_INVALID = 0,
    RTC_DRIFT_LEARN_BASELINE,
    RTC_DRIFT_LEARN_DUPLICATE,
    RTC_DRIFT_LEARN_INSUFFICIENT,
    RTC_DRIFT_LEARN_INTERVAL,
    RTC_DRIFT_LEARN_RATE,
    RTC_DRIFT_LEARN_UNCERTAINTY,
    RTC_DRIFT_LEARN_PENDING,
    RTC_DRIFT_LEARN_INCONSISTENT,
    RTC_DRIFT_LEARN_PUBLISHED,
    RTC_DRIFT_LEARN_SOURCE,
    RTC_DRIFT_LEARN_CONTINUITY,
    RTC_DRIFT_LEARN_OVERFLOW,
    RTC_DRIFT_LEARN_NO_NEW_SLEEP
} rtc_drift_learn_result_t;

bool rtc_drift_time_valid(const rtc_drift_time_t *time);
bool rtc_drift_time_add(const rtc_drift_time_t *a, const rtc_drift_time_t *b,
    rtc_drift_time_t *result);
bool rtc_drift_time_sub(const rtc_drift_time_t *a, const rtc_drift_time_t *b,
    rtc_drift_time_t *result);
bool rtc_drift_scale_time(const rtc_drift_time_t *raw, int32_t ppm,
    rtc_drift_ratio_t *ratio, rtc_drift_time_t *corrected);
bool rtc_drift_learning_reset(rtc_drift_learning_t *state, uint32_t source,
    uint32_t continuity, int32_t saved_ppm, bool saved_qualified);
rtc_drift_learn_result_t rtc_drift_learn(rtc_drift_learning_t *state,
    const rtc_drift_reference_t *reference);

void rtc_drift_training_clear(rtc_drift_training_t *training);
bool rtc_drift_training_add(rtc_drift_training_t *training, int32_t temp_mc, uint32_t period_q19);
bool rtc_drift_model_fit(const rtc_drift_training_t *training, rtc_drift_model_t *model);
bool rtc_drift_model_validate(const rtc_drift_model_t *model);
bool rtc_drift_model_evaluate(const rtc_drift_model_t *model, int32_t temp_mc, double *period_q19);
rtc_drift_selection_t rtc_drift_select(int64_t now_us, const rtc_drift_direct_t *direct,
    const rtc_drift_temperature_t *temperature, const rtc_drift_model_t *model,
    uint32_t *period_q19);

#endif
