/* 真实公共策略头的逻辑回归，不假定主机测试能证明物理 GPIO 脉宽。 */
#include <assert.h>
#include <stdio.h>
#include "rtc_wake_policy.h"

int main(void)
{
    assert(rtc_wake_should_maintain(true, false, true, true));
    assert(!rtc_wake_should_maintain(false, false, true, true));
    assert(!rtc_wake_should_maintain(true, true, true, true));
    assert(!rtc_wake_should_maintain(true, false, false, true));
    assert(!rtc_wake_should_maintain(true, false, true, false));
    rtc_wake_watch_t watch;
    rtc_wake_watch_begin(&watch, 10, false);
    assert(rtc_wake_watch_poll(&watch, 11, false));
    assert(watch.max_gap_upper_us == 40);
    assert(!rtc_wake_watch_poll(&watch, 12, true));
    assert(watch.key_seen);
    assert(!rtc_wake_watch_poll(&watch, 13, false));
    rtc_wake_watch_begin(&watch, RTC_DRIFT_TICK_MASK - 1, false);
    assert(rtc_wake_watch_poll(&watch, 1, false));
    assert(watch.max_gap_upper_us == 80);
    rtc_wake_watch_begin(&watch, 0, false);
    assert(rtc_wake_watch_poll(&watch, 398, false));
    assert(watch.max_gap_upper_us == 7980);
    assert(!rtc_wake_watch_poll(&watch, 797, false));
    assert(watch.gap_exceeded && watch.max_gap_upper_us == RTC_WAKE_GAP_LIMIT_US);
    assert(!rtc_wake_watch_poll(&watch, 798, false));
    rtc_wake_watch_begin(&watch, 0, false);
    assert(!rtc_wake_watch_poll(&watch, RTC_DRIFT_TICK_HALF, false));
    assert(watch.max_gap_upper_us == UINT32_MAX);
    rtc_wake_watch_begin(&watch, 3, true);
    assert(!rtc_wake_watch_poll(&watch, 3, false));
    puts("RTC timer routing, sticky cancellation, wrap and gap bound: PASS");
    return 0;
}
