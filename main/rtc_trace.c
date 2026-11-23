#include "rtc_trace.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
RTC_NOINIT_ATTR rtc_trace_store_t rtc_trace_store;
#endif

static uint32_t checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void seal_header(void)
{
    rtc_trace_store.header_checksum = checksum(&rtc_trace_store,
        offsetof(rtc_trace_store_t, header_checksum));
}

bool rtc_trace_record_valid(const rtc_trace_record_t *record)
{
    return record->sequence && record->kind >= RTC_TRACE_RESET &&
        record->kind <= RTC_TRACE_UNCERTAIN_REFERENCE &&
        record->checksum == checksum(record, offsetof(rtc_trace_record_t, checksum));
}

void rtc_trace_init(void)
{
    if (rtc_trace_store.magic != RTC_TRACE_MAGIC || rtc_trace_store.version != RTC_TRACE_VERSION) {
        memset(&rtc_trace_store, 0, sizeof rtc_trace_store);
        rtc_trace_store.magic = RTC_TRACE_MAGIC;
        rtc_trace_store.version = RTC_TRACE_VERSION;
    } else {
        /* Recover a record completed before a reset interrupted header update.
         * This runs once per boot; no scan on the clock's hot path. */
        uint32_t latest = rtc_trace_store.header_checksum == checksum(&rtc_trace_store,
            offsetof(rtc_trace_store_t, header_checksum)) ? rtc_trace_store.sequence : 0;
        for (unsigned i = 0; i < RTC_TRACE_CAPACITY; ++i) {
            const rtc_trace_record_t *row = &rtc_trace_store.records[i];
            if (rtc_trace_record_valid(row) && (row->sequence - 1) % RTC_TRACE_CAPACITY == i &&
                row->sequence > latest) latest = row->sequence;
        }
        rtc_trace_store.sequence = latest;
    }
    seal_header();
}

void rtc_trace_append(const rtc_trace_record_t *record)
{
    /* Saturate after billions of events; never change timekeeping on failure. */
    if (rtc_trace_store.sequence == UINT32_MAX) return;
    rtc_trace_record_t next = *record;
    next.sequence = rtc_trace_store.sequence + 1;
    next.checksum = checksum(&next, offsetof(rtc_trace_record_t, checksum));
    rtc_trace_store.records[(next.sequence - 1) % RTC_TRACE_CAPACITY] = next;
    rtc_trace_store.sequence = next.sequence;
    seal_header();
}

void rtc_trace_begin(const rtc_trace_record_t *record)
{
    rtc_trace_cancel();
    rtc_trace_store.pending = *record;
    rtc_trace_store.pending.sequence = 1;
    rtc_trace_store.pending.checksum = checksum(&rtc_trace_store.pending,
        offsetof(rtc_trace_record_t, checksum));
    rtc_trace_store.wake_tick = UINT64_MAX;
    rtc_trace_store.wake_tick_inverse = UINT64_MAX;
}

void rtc_trace_cancel(void)
{
    if (rtc_trace_record_valid(&rtc_trace_store.pending)) {
        rtc_trace_record_t gap = rtc_trace_store.pending;
        gap.kind = RTC_TRACE_GAP;
        gap.end_tick = UINT64_MAX;
        gap.hrt_us = gap.wall_after_us = -1;
        const uint64_t wake = rtc_trace_store.wake_tick;
        gap.wake_tick = wake == ~rtc_trace_store.wake_tick_inverse ? wake : UINT64_MAX;
        rtc_trace_append(&gap);
    }
    rtc_trace_store.pending.sequence = 0;
}

void rtc_trace_end(const rtc_trace_record_t *record, uint64_t start_tick, uint32_t period_q19)
{
    rtc_trace_record_t next = *record;
    const rtc_trace_record_t *pending = &rtc_trace_store.pending;
    if (rtc_trace_record_valid(pending) && pending->kind == RTC_TRACE_SLEEP &&
        pending->start_tick == start_tick && pending->period_q19 == period_q19 &&
        pending->continuity == record->continuity && pending->source == record->source) {
        next.kind = RTC_TRACE_SLEEP;
        next.start_tick = pending->start_tick;
        next.wall_before_us = pending->wall_before_us;
        next.commit_hrt_us = pending->commit_hrt_us;
        next.period_q19 = pending->period_q19;
        next.ppm = pending->ppm;
        next.light_raw_us = pending->light_raw_us;
        next.light_sdk_us = pending->light_sdk_us;
        next.light_offset_us = pending->light_offset_us;
        const uint64_t wake = rtc_trace_store.wake_tick;
        next.wake_tick = wake == ~rtc_trace_store.wake_tick_inverse ? wake : UINT64_MAX;
    } else {
        next.kind = RTC_TRACE_GAP;
        next.start_tick = start_tick;
        next.period_q19 = period_q19;
        next.wake_tick = UINT64_MAX;
    }
    rtc_trace_append(&next);
    rtc_trace_store.pending.sequence = 0;
}
