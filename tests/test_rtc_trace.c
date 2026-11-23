#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtc_trace.h"

rtc_trace_store_t rtc_trace_store;

int main(void)
{
    memset(&rtc_trace_store, 0xa5, sizeof rtc_trace_store);
    rtc_trace_init();
    assert(rtc_trace_store.sequence == 0);
    rtc_trace_record_t row = {.kind = RTC_TRACE_RESET, .ppm = -870};
    rtc_trace_append(&row);
    assert(rtc_trace_record_valid(&rtc_trace_store.records[0]));
    assert(rtc_trace_store.records[0].sequence == 1);
    rtc_trace_store.records[0].raw_us ^= 1;
    assert(!rtc_trace_record_valid(&rtc_trace_store.records[0]));
    for (unsigned i = 0; i < RTC_TRACE_CAPACITY + 5; ++i) {
        row.raw_us = i;
        rtc_trace_append(&row);
    }
    const uint32_t sequence = rtc_trace_store.sequence;
    rtc_trace_init();
    assert(rtc_trace_store.sequence == sequence);
    rtc_trace_store.header_checksum ^= 1;
    rtc_trace_init();
    assert(rtc_trace_store.sequence == sequence);
    rtc_trace_store.records[(sequence - 1) % RTC_TRACE_CAPACITY].checksum ^= 1;
    rtc_trace_init();
    assert(rtc_trace_store.sequence == sequence); // Never reuse a damaged event's number.
    row = (rtc_trace_record_t){.kind = RTC_TRACE_SLEEP, .start_tick = 77, .period_q19 = 7600000};
    rtc_trace_begin(&row);
    assert(rtc_trace_record_valid(&rtc_trace_store.pending));
    rtc_trace_store.wake_tick = 90;
    rtc_trace_store.wake_tick_inverse = ~UINT64_C(90);
    rtc_trace_record_t complete = {.end_tick = 100, .raw_us = 333, .raw_fraction = 19};
    rtc_trace_end(&complete, 77, 7600000);
    rtc_trace_record_t *last = &rtc_trace_store.records[(rtc_trace_store.sequence - 1) % RTC_TRACE_CAPACITY];
    assert(last->kind == RTC_TRACE_SLEEP && last->wake_tick == 90);
    assert(last->start_tick == 77 && last->end_tick == 100 && last->raw_us == 333);
    assert(last->raw_fraction == 19 && rtc_trace_record_valid(last));
    assert(!rtc_trace_record_valid(&rtc_trace_store.pending));
    rtc_trace_begin(&row);
    rtc_trace_cancel();
    rtc_trace_end(&complete, 77, 7600000);
    last = &rtc_trace_store.records[(rtc_trace_store.sequence - 1) % RTC_TRACE_CAPACITY];
    assert(last->kind == RTC_TRACE_GAP);
    rtc_trace_begin(&row);
    rtc_trace_end(&complete, 78, 7600000);
    last = &rtc_trace_store.records[(rtc_trace_store.sequence - 1) % RTC_TRACE_CAPACITY];
    assert(last->kind == RTC_TRACE_GAP);
    rtc_trace_begin(&row);
    rtc_trace_store.wake_tick = 99;
    rtc_trace_store.wake_tick_inverse = 0;
    rtc_trace_end(&complete, 77, 7600000);
    last = &rtc_trace_store.records[(rtc_trace_store.sequence - 1) % RTC_TRACE_CAPACITY];
    assert(last->wake_tick == UINT64_MAX);
    puts("rtc_trace: PASS");
}
