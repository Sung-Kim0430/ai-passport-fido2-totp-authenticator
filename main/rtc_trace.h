#ifndef RTC_TRACE_H
#define RTC_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTC_TRACE_CAPACITY 44u
#define RTC_TRACE_MAGIC UINT32_C(0x52545431)
#define RTC_TRACE_VERSION 1u

enum {
    RTC_TRACE_RESET = 1,
    RTC_TRACE_SLEEP,
    RTC_TRACE_REFERENCE,
    RTC_TRACE_GAP,
    RTC_TRACE_UNCERTAIN_REFERENCE
};

/* Numeric diagnostics only. Totals use the application's accounting origin;
 * light totals are boot-local snapshots at commit/reference. A sleep's HRT
 * and wall_after refer to its restoration capture, not record publication. */
typedef struct {
    uint32_t sequence, kind;
    uint64_t start_tick, wake_tick, end_tick;
    int64_t hrt_us, commit_hrt_us, wall_before_us, wall_after_us;
    uint64_t raw_us, corrected_us, awake_us;
    uint64_t light_raw_us, light_sdk_us;
    int64_t light_offset_us;
    uint32_t period_q19;
    int32_t ppm;
    uint32_t raw_fraction, continuity, source, checksum;
} rtc_trace_record_t;

typedef struct {
    uint32_t magic, version, sequence, header_checksum;
    rtc_trace_record_t records[RTC_TRACE_CAPACITY];
    rtc_trace_record_t pending;
    volatile uint64_t wake_tick, wake_tick_inverse;
} rtc_trace_store_t;

_Static_assert(sizeof(rtc_trace_record_t) == 136, "trace wire record");
_Static_assert(offsetof(rtc_trace_record_t, checksum) == 132, "trace checksum offset");
_Static_assert(sizeof(rtc_trace_store_t) == 6152, "trace RTC memory budget");

extern rtc_trace_store_t rtc_trace_store;
void rtc_trace_init(void);
bool rtc_trace_record_valid(const rtc_trace_record_t *record);
void rtc_trace_append(const rtc_trace_record_t *record);
void rtc_trace_begin(const rtc_trace_record_t *record);
void rtc_trace_end(const rtc_trace_record_t *record, uint64_t start_tick, uint32_t period_q19);
void rtc_trace_cancel(void);

#endif
