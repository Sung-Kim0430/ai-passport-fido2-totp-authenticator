#ifndef RTC_MODE_H
#define RTC_MODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RTC_MODE_ECO = 0,
    RTC_MODE_FREQUENT = 1,
    RTC_MODE_XTAL = 2,
    RTC_MODE_COUNT = 3
} rtc_mode_t;

static inline bool rtc_mode_valid(rtc_mode_t mode)
{
    return (unsigned)mode < RTC_MODE_COUNT;
}

static inline bool rtc_mode_deep_sleep(rtc_mode_t mode)
{
    return mode == RTC_MODE_ECO || mode == RTC_MODE_FREQUENT;
}

static inline bool rtc_mode_light_sleep(rtc_mode_t mode)
{
    return rtc_mode_deep_sleep(mode);
}

static inline uint64_t rtc_mode_interval_us(rtc_mode_t mode, bool first)
{
    if (!rtc_mode_deep_sleep(mode)) return 0;
    if (first) return UINT64_C(120000000);
    return mode == RTC_MODE_ECO ? UINT64_C(900000000) : UINT64_C(180000000);
}

#endif
