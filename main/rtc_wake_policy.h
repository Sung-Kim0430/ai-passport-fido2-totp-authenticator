#ifndef RTC_WAKE_POLICY_H
#define RTC_WAKE_POLICY_H

#include "rtc_drift.h"

#define RTC_WAKE_GAP_LIMIT_US UINT32_C(8000)

typedef struct {
    uint64_t last_tick;
    uint32_t max_gap_upper_us;
    bool key_seen;
    bool gap_exceeded;
} rtc_wake_watch_t;

/* GPIO、混合原因、无效会话都走完整启动；定时原因不能压过用户输入。 */
RTC_DRIFT_INLINE bool rtc_wake_should_maintain(bool timer_only, bool key_seen, bool session_valid, bool enabled)
{
    return timer_only && !key_seen && session_valid && enabled;
}
RTC_DRIFT_INLINE void rtc_wake_watch_begin(rtc_wake_watch_t *watch, uint64_t tick, bool key_seen)
{
    watch->last_tick = tick & RTC_DRIFT_TICK_MASK;
    watch->max_gap_upper_us = 0;
    watch->key_seen = key_seen;
    watch->gap_exceeded = false;
}
RTC_DRIFT_INLINE bool rtc_wake_watch_poll(rtc_wake_watch_t *watch, uint64_t tick, bool key_seen)
{
    tick &= RTC_DRIFT_TICK_MASK;
    const uint64_t delta = (tick - watch->last_tick) & RTC_DRIFT_TICK_MASK;
    /* 每 tick 最多按允许周期上界 20us 计算，并补一个量化不确定 tick。 */
    const uint32_t tick_upper_us = RTC_DRIFT_PERIOD_MAX >> 19;
    uint32_t upper = UINT32_MAX;
    if (delta < RTC_DRIFT_TICK_HALF && delta < UINT32_MAX / tick_upper_us) {
        upper = (uint32_t)(delta + 1) * tick_upper_us;
    }
    if (upper > watch->max_gap_upper_us) watch->max_gap_upper_us = upper;
    if (upper >= RTC_WAKE_GAP_LIMIT_US) watch->gap_exceeded = true;
    watch->key_seen = watch->key_seen || key_seen;
    watch->last_tick = tick;
    return !watch->key_seen && !watch->gap_exceeded;
}

#endif
