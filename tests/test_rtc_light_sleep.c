#include <assert.h>
#include <stdio.h>
#include "rtc_light_sleep.h"

static void assert_atomic_failure(rtc_light_sleep_snapshot_t state, uint64_t start,
    uint64_t end, uint32_t sdk, uint32_t direct, int64_t exit)
{
    rtc_light_sleep_snapshot_t before = state;
    assert(!rtc_light_sleep_record(&state, start, end, sdk, direct, exit));
    assert(state.fault);
    assert(state.raw_total.us == before.raw_total.us);
    assert(state.raw_total.fraction_q19 == before.raw_total.fraction_q19);
    assert(state.sdk_total_us == before.sdk_total_us);
    assert(state.wall_offset_total_us == before.wall_offset_total_us);
    assert(state.uncertainty_total_us == before.uncertainty_total_us);
    assert(state.sleep_count == before.sleep_count);
    assert(state.last_exit_hrt_us == before.last_exit_hrt_us);
}

int main(void)
{
    const uint32_t p = 6 * RTC_DRIFT_Q19 + RTC_DRIFT_Q19 / 2;
    rtc_light_sleep_snapshot_t state = {0};
    assert(rtc_light_sleep_record(&state, 10, 11, p, p, 20));
    assert(state.raw_total.us == 6 && state.raw_total.fraction_q19 == RTC_DRIFT_Q19 / 2);
    assert(state.sdk_total_us == 6 && state.uncertainty_total_us == 17);
    assert(rtc_light_sleep_record(&state, RTC_DRIFT_TICK_MASK, 0, p, p, 30));
    assert(state.raw_total.us == 13 && state.raw_total.fraction_q19 == 0);
    assert(state.sdk_total_us == 12 && state.sleep_count == 2);
    assert_atomic_failure(state, 0, RTC_DRIFT_TICK_HALF, p, p, 40);
    assert_atomic_failure(state, 0, 1, 0, p, 40);
    assert_atomic_failure(state, 0, 1, p, 0, 40);
    assert_atomic_failure(state, 0, 1, p, p, 29);
    assert_atomic_failure(state, RTC_DRIFT_TICK_MASK + 1, 1, p, p, 40);
    state.raw_total.us = RTC_DRIFT_MAX_ELAPSED_US;
    assert_atomic_failure(state, 0, 1, p, p, 40);
    state = (rtc_light_sleep_snapshot_t){.sdk_total_us = RTC_DRIFT_MAX_ELAPSED_US};
    assert_atomic_failure(state, 0, 1, p, p, 40);
    state = (rtc_light_sleep_snapshot_t){.uncertainty_total_us = RTC_DRIFT_MAX_ELAPSED_US};
    assert_atomic_failure(state, 0, 1, p, p, 40);
    state = (rtc_light_sleep_snapshot_t){.sleep_count = UINT32_MAX};
    assert_atomic_failure(state, 0, 1, p, p, 40);
    state = (rtc_light_sleep_snapshot_t){.fault = true};
    assert_atomic_failure(state, 0, 1, p, p, 40);
    assert(!rtc_light_sleep_record(NULL, 0, 1, p, p, 40));
    /* Independent integer oracle: fraction retention, varying direct
     * periods, per-request SDK truncation, and RTC wraparound. */
    state = (rtc_light_sleep_snapshot_t){0};
    uint64_t tick = RTC_DRIFT_TICK_MASK - 100;
    uint64_t expected_q19 = 0, expected_sdk = 0, expected_uncertainty = 0;
    uint32_t random = 12345;
    for (unsigned i = 1; i <= 10000; ++i) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        const uint32_t direct = RTC_DRIFT_PERIOD_MIN + random %
            (RTC_DRIFT_PERIOD_MAX - RTC_DRIFT_PERIOD_MIN + 1);
        const uint32_t sdk = RTC_DRIFT_PERIOD_MIN + (random >> 1) %
            (RTC_DRIFT_PERIOD_MAX - RTC_DRIFT_PERIOD_MIN + 1);
        const uint64_t delta = 1 + random % 10000;
        const uint64_t end = (tick + delta) & RTC_DRIFT_TICK_MASK;
        expected_q19 += delta * direct;
        expected_sdk += delta * sdk / RTC_DRIFT_Q19;
        expected_uncertainty += ((uint64_t)2 * direct + RTC_DRIFT_Q19 - 1) /
            RTC_DRIFT_Q19 + 4;
        assert(rtc_light_sleep_record(&state, tick, end, sdk, direct, (int64_t)i));
        assert(state.raw_total.us == expected_q19 / RTC_DRIFT_Q19);
        assert(state.raw_total.fraction_q19 == expected_q19 % RTC_DRIFT_Q19);
        assert(state.sdk_total_us == expected_sdk);
        assert(state.uncertainty_total_us == expected_uncertainty);
        tick = end;
    }
    puts("RTC light sleep accumulator: PASS");
    return 0;
}
