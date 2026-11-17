#ifndef RTC_LIGHT_SLEEP_H
#define RTC_LIGHT_SLEEP_H
#include "rtc_drift.h"

/* Boot-local cumulative exposure; snapshots and their HRT stamp are atomic. */
typedef struct {
    rtc_drift_time_t raw_total;
    uint64_t sdk_total_us, uncertainty_total_us;
    int64_t last_exit_hrt_us, hrt_us;
    /* Additional libc wall-time displacement caused by successful sleep. */
    int64_t wall_offset_total_us;
    uint32_t sleep_count, reject_count;
    bool fault;
} rtc_light_sleep_snapshot_t;

/* Record only successful SDK sleep requests. Direct exposure carries its Q19
 * fraction across intervals; SDK compensation truncates EACH interval exactly
 * as sleep_modes.c does. Existing ledger bounds avoid a wide tick*period
 * multiplication. Any invalid/ambiguous/overflowing record sets a sticky fault
 * and preserves every numerical field. The owner serializes access. */
RTC_DRIFT_INLINE bool rtc_light_sleep_record(rtc_light_sleep_snapshot_t *state,
    uint64_t start_tick, uint64_t end_tick, uint32_t sdk_period_q19,
    uint32_t direct_period_q19, int64_t exit_hrt_us)
{
    if (!state) return false;
    rtc_drift_ledger_t raw, sdk;
    uint64_t raw_us, sdk_us;
    uint32_t raw_fraction, sdk_fraction;
    if (state->fault || start_tick > RTC_DRIFT_TICK_MASK ||
        end_tick > RTC_DRIFT_TICK_MASK || exit_hrt_us < 0 ||
        exit_hrt_us < state->last_exit_hrt_us ||
        state->sleep_count == UINT32_MAX ||
        state->raw_total.us > RTC_DRIFT_MAX_ELAPSED_US ||
        state->raw_total.fraction_q19 >= RTC_DRIFT_Q19 ||
        state->sdk_total_us > RTC_DRIFT_MAX_ELAPSED_US ||
        state->uncertainty_total_us > RTC_DRIFT_MAX_ELAPSED_US ||
        !rtc_drift_ledger_start(&raw, start_tick, direct_period_q19) ||
        !rtc_drift_ledger_start(&sdk, start_tick, sdk_period_q19)) {
        state->fault = true;
        return false;
    }
    raw.elapsed_us = state->raw_total.us;
    raw.remainder_q19 = state->raw_total.fraction_q19;
    const uint64_t uncertainty =
        ((uint64_t)2 * direct_period_q19 + RTC_DRIFT_Q19 - 1) / RTC_DRIFT_Q19 + 4;
    if (!rtc_drift_ledger_value_at(&raw, end_tick, &raw_us, &raw_fraction) ||
        !rtc_drift_ledger_value_at(&sdk, end_tick, &sdk_us, &sdk_fraction) ||
        sdk_us > RTC_DRIFT_MAX_ELAPSED_US - state->sdk_total_us ||
        uncertainty > RTC_DRIFT_MAX_ELAPSED_US - state->uncertainty_total_us) {
        state->fault = true;
        return false;
    }
    state->raw_total.us = raw_us;
    state->raw_total.fraction_q19 = raw_fraction;
    state->sdk_total_us += sdk_us;
    state->uncertainty_total_us += uncertainty;
    state->last_exit_hrt_us = exit_hrt_us;
    ++state->sleep_count;
    return true;
}

/* Normal task only; initialize before enabling automatic sleep. This module
 * owns the SDK sleep-debug context. Registration failure rolls back hooks. */
bool rtc_light_sleep_platform_start(uint32_t period_q19, uint32_t source);
/* A freshly completed DIRECT calibration only. Never feed a prediction here.
 * Expiry skips sleep temporarily; invalid/source changes permanently fault. */
void rtc_light_sleep_platform_set_period(uint32_t period_q19);
/* Irreversible for this boot; retained callbacks continue to guard PM. */
void rtc_light_sleep_platform_inhibit(void);
rtc_light_sleep_snapshot_t rtc_light_sleep_platform_snapshot(void);
#endif
