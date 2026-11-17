#include "rtc_history.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static rtc_history_record_t sample(void)
{
    return (rtc_history_record_t){
        .source = 2, .sequence = 1, .epoch = 1,
        .utc_end_us = UINT64_C(1700000000000000),
        .raw = {RTC_HISTORY_MIN_RAW_US, 0},
        .utc_delta_us = RTC_HISTORY_MIN_RAW_US
    };
}

static void roundtrip(void)
{
    rtc_history_record_t r = sample(), decoded = {0};
    uint8_t wire[RTC_HISTORY_WIRE_SIZE], again[RTC_HISTORY_WIRE_SIZE];
    assert(rtc_history_encode(&r, wire, sizeof wire));
    assert(rtc_history_decode(wire, sizeof wire, &decoded));
    assert(rtc_history_encode(&decoded, again, sizeof again));
    assert(memcmp(wire, again, sizeof wire) == 0);
    assert(decoded.raw.us == r.raw.us && decoded.sequence == 1);
}

static void invalid(rtc_history_record_t r)
{
    uint8_t wire[80], before[80];
    memset(wire, 0xa5, sizeof wire);
    memcpy(before, wire, sizeof wire);
    double rate = 42;
    assert(!rtc_history_record_valid(&r));
    assert(!rtc_history_rate_ppm(&r, &rate) && rate == 42);
    assert(!rtc_history_encode(&r, wire, sizeof wire));
    assert(memcmp(before, wire, sizeof wire) == 0);
}

static void validation(void)
{
    rtc_history_record_t r = sample();
    double rate = 42;
    assert(rtc_history_record_valid(&r));
    assert(rtc_history_rate_ppm(&r, &rate) && rate == 0);
    assert(!rtc_history_record_valid(NULL));
    assert(!rtc_history_rate_ppm(&r, NULL));
#define BAD(field, value) do { r = sample(); r.field = (value); invalid(r); } while (0)
    BAD(flags, 16);
    BAD(source, 1);
    BAD(source, UINT32_MAX);
    BAD(epoch, 0);
    BAD(sequence, 0);
    BAD(raw.fraction_q19, RTC_DRIFT_Q19);
    BAD(raw.fraction_q19, UINT32_MAX);
    BAD(raw.us, RTC_HISTORY_MIN_RAW_US - 1);
    BAD(raw.us, UINT64_MAX);
    BAD(utc_end_us, RTC_HISTORY_UTC_MIN_US - 1);
    BAD(utc_end_us, RTC_HISTORY_UTC_MAX_US + 1);
    BAD(utc_end_us, RTC_HISTORY_UTC_MIN_US);
    BAD(utc_end_us, UINT64_MAX);
    BAD(utc_delta_us, 0);
    BAD(utc_delta_us, UINT64_MAX);
    BAD(utc_delta_us, RTC_HISTORY_MAX_WINDOW_US + 1);
    BAD(awake_delta_us, RTC_HISTORY_MIN_RAW_US);
    BAD(awake_delta_us, UINT64_MAX);
    BAD(combined_uncertainty_us, RTC_HISTORY_MIN_RAW_US / 10000 + 1);
    BAD(combined_uncertainty_us, UINT64_MAX);
    BAD(temperature_centi_c, 1);
    BAD(battery_mv, 1);
    BAD(temp_age_s, 1);
    BAD(battery_age_s, 1);
    r = sample(); r.flags = RTC_HISTORY_TEMP_VALID;
    r.temperature_centi_c = -1000; assert(rtc_history_record_valid(&r));
    r.temperature_centi_c = 8000; assert(rtc_history_record_valid(&r));
    r.temperature_centi_c = -1001; invalid(r);
    r.temperature_centi_c = 8001; invalid(r);
    r.temperature_centi_c = INT16_MIN; invalid(r);
    r.temperature_centi_c = INT16_MAX; invalid(r);
    r = sample(); r.flags = RTC_HISTORY_BATTERY_VALID; invalid(r);
    r.battery_mv = 1; assert(rtc_history_record_valid(&r));
    r.battery_mv = UINT16_MAX; assert(rtc_history_record_valid(&r));
    r = sample(); r.utc_delta_us = UINT64_C(100000000000);
    r.raw.us = r.utc_delta_us * 51 / 50;
    assert(rtc_history_rate_ppm(&r, &rate) && fabs(rate - 20000) < 1e-8);
    r.raw.fraction_q19 = 1; invalid(r);
    r.raw.us = r.utc_delta_us * 49 / 50; r.raw.fraction_q19 = 0;
    assert(rtc_history_rate_ppm(&r, &rate) && fabs(rate + 20000) < 1e-8);
    --r.raw.us; r.raw.fraction_q19 = RTC_DRIFT_Q19 - 1; invalid(r);
    r = sample(); r.utc_delta_us = RTC_HISTORY_MAX_WINDOW_US;
    r.raw.us = r.utc_delta_us * 51 / 50;
    assert(rtc_history_record_valid(&r));
    r.raw.fraction_q19 = 1; invalid(r);
    r.raw.us = r.utc_delta_us * 49 / 50; r.raw.fraction_q19 = 0;
    assert(rtc_history_record_valid(&r));
    --r.raw.us; r.raw.fraction_q19 = RTC_DRIFT_Q19 - 1; invalid(r);
    r = sample(); r.utc_end_us = RTC_HISTORY_UTC_MIN_US + r.utc_delta_us;
    assert(rtc_history_record_valid(&r));
    --r.utc_end_us; invalid(r);
    r = sample(); r.utc_end_us = RTC_HISTORY_UTC_MAX_US;
    assert(rtc_history_record_valid(&r));
    r = sample(); r.raw.fraction_q19 = RTC_DRIFT_Q19 / 2;
    assert(rtc_history_rate_ppm(&r, &rate) && rate > 0 && rate < 0.001);
    r.raw.us -= 1;
    assert(rtc_history_rate_ppm(&r, &rate) == false);
    r = sample(); r.raw.us += 100; r.utc_delta_us += 101;
    r.raw.fraction_q19 = RTC_DRIFT_Q19 / 2;
    assert(rtc_history_rate_ppm(&r, &rate) && rate < 0);
    assert(fabs(rate - (-0.5 * 1000000.0 / r.utc_delta_us)) < 1e-15);
    r.utc_delta_us = UINT64_C(100000000001);
    r.raw.us = UINT64_C(98000000000);
    r.raw.fraction_q19 = RTC_DRIFT_Q19 * 49 / 50;
    invalid(r);
    ++r.raw.fraction_q19; assert(rtc_history_record_valid(&r));
    r.raw.us = UINT64_C(102000000001);
    r.raw.fraction_q19 = RTC_DRIFT_Q19 / 50;
    assert(rtc_history_record_valid(&r));
    ++r.raw.fraction_q19; invalid(r);
#undef BAD
}

static const uint8_t golden[80] = {
    0x01, 0x00, 0x0f, 0x00, 0x02, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x40, 0x1e, 0x18, 0x24, 0x0a, 0x06, 0x00,
    0x00, 0xd8, 0x75, 0x07, 0x05, 0x00, 0x00, 0x00,
    0xd2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xd2, 0xdc, 0x75, 0x07, 0x05, 0x00, 0x00, 0x00,
    0x80, 0xf5, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x39, 0x30, 0x00, 0x00, 0x18, 0xfc, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x48, 0x13, 0x76, 0xd6
};

static void fix_crc(uint8_t *wire)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 76; ++i) {
        crc ^= wire[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = crc & 1 ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) wire[76 + i] = (uint8_t)(crc >> (8 * i));
}

static void codec(void)
{
    rtc_history_record_t r = sample(), decoded, before;
    r.flags = 15; r.sequence = UINT64_MAX; r.epoch = UINT64_MAX;
    r.awake_delta_us = 1234; r.utc_delta_us += 1234;
    r.combined_uncertainty_us = 2160000; r.raw.fraction_q19 = 12345;
    r.temperature_centi_c = -1000; r.battery_mv = UINT16_MAX;
    r.temp_age_s = UINT16_MAX; r.battery_age_s = UINT16_MAX;
    uint8_t wire[81], copy[81];
    assert(rtc_history_encode(&r, wire, 80));
    assert(memcmp(wire, golden, 80) == 0);
    assert(rtc_history_decode(golden, 80, &decoded));
    assert(decoded.temperature_centi_c == -1000 && decoded.battery_mv == UINT16_MAX);
    assert(decoded.epoch == UINT64_MAX && decoded.raw.fraction_q19 == 12345);
    memset(&before, 0xa5, sizeof before);
    for (unsigned bit = 0; bit < 640; ++bit) {
        memcpy(wire, golden, 80); wire[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        memcpy(&decoded, &before, sizeof decoded);
        assert(!rtc_history_decode(wire, 80, &decoded));
        assert(memcmp(&decoded, &before, sizeof decoded) == 0);
    }
    const unsigned offsets[] = {0, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64, 68};
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        memcpy(wire, golden, 80);
        if (offsets[i] == 8 || offsets[i] == 16) memset(wire + offsets[i], 0, 8);
        else wire[offsets[i]] ^= 0x80;
        if (offsets[i] == 24 || offsets[i] == 32 || offsets[i] == 40 ||
            offsets[i] == 48 || offsets[i] == 56) memset(wire + offsets[i], 0xff, 8);
        if (offsets[i] == 64) memset(wire + 64, 0xff, 4);
        if (offsets[i] == 68) { wire[68] = 0xff; wire[69] = 0x7f; }
        fix_crc(wire);
        memcpy(&decoded, &before, sizeof decoded);
        assert(!rtc_history_decode(wire, 80, &decoded));
        assert(memcmp(&decoded, &before, sizeof decoded) == 0);
    }
    for (size_t size = 0; size <= 81; ++size) {
        if (size == 80) continue;
        memset(wire, 0xa5, sizeof wire); memcpy(copy, wire, sizeof wire);
        assert(!rtc_history_encode(&r, wire, size));
        assert(memcmp(wire, copy, sizeof wire) == 0);
        memcpy(&decoded, &before, sizeof decoded);
        assert(!rtc_history_decode(wire, size, &decoded));
        assert(memcmp(&decoded, &before, sizeof decoded) == 0);
    }
    assert(!rtc_history_encode(NULL, wire, 80));
    assert(!rtc_history_encode(&r, NULL, 80));
    assert(!rtc_history_decode(NULL, 80, &decoded));
    assert(!rtc_history_decode(golden, 80, NULL));
    for (unsigned i = 0; i < 1000; ++i) {
        r = sample(); r.source = i % 2 ? 0 : 2;
        r.sequence = i + 1; r.epoch = UINT64_MAX - i;
        r.utc_delta_us += (uint64_t)i * 1000000;
        r.raw.us = r.utc_delta_us + (i % 2 ? i * 11 : 0);
        r.raw.fraction_q19 = (i * 7919) % RTC_DRIFT_Q19;
        assert(rtc_history_encode(&r, wire, 80));
        assert(rtc_history_decode(wire, 80, &decoded));
        assert(decoded.raw.us == r.raw.us && decoded.raw.fraction_q19 == r.raw.fraction_q19);
        assert(rtc_history_encode(&decoded, copy, 80));
        assert(memcmp(copy, wire, 80) == 0);
        wire[(i * 17) % 80] ^= (uint8_t)(1u << (i % 8));
        assert(!rtc_history_decode(wire, 80, &decoded));
        r.raw.fraction_q19 = RTC_DRIFT_Q19 + i; invalid(r);
    }
}

static rtc_history_record_t numbered(uint64_t n)
{
    rtc_history_record_t r = sample();
    r.sequence = n;
    r.utc_end_us += n * r.utc_delta_us;
    return r;
}

static void append_rejected(rtc_history_t *h, const rtc_history_record_t *r)
{
    rtc_history_t before;
    memcpy(&before, h, sizeof before);
    assert(!rtc_history_append(h, r));
    assert(memcmp(h, &before, sizeof before) == 0);
}

static void ring(void)
{
    rtc_history_t h;
    rtc_history_record_t r, got, sentinel;
    rtc_history_init(NULL);
    rtc_history_init(&h);
    assert(h.count == 0 && h.generation == 0 && h.next == 0);
    memset(&sentinel, 0xa5, sizeof sentinel); memcpy(&got, &sentinel, sizeof got);
    assert(!rtc_history_get(&h, 0, &got));
    assert(memcmp(&got, &sentinel, sizeof got) == 0);
    assert(!rtc_history_get(NULL, 0, &got));
    assert(!rtc_history_append(NULL, &got));
    append_rejected(&h, NULL);
    for (uint64_t n = 1; n <= 101; ++n) {
        r = numbered(n);
        assert(rtc_history_append(&h, &r));
        assert(h.count == (n > 100 ? 100 : n) && h.generation == n);
        assert(rtc_history_get(&h, 0, &got));
        assert(got.sequence == (n > 100 ? n - 99 : 1));
        assert(rtc_history_get(&h, h.count - 1, &got) && got.sequence == n);
    }
    for (size_t i = 0; i < 100; ++i) {
        assert(rtc_history_get(&h, i, &got)); assert(got.sequence == i + 2);
    }
    assert(!rtc_history_get(&h, 100, &got));
    assert(!rtc_history_get(&h, SIZE_MAX, &got));
    assert(!rtc_history_get(&h, 0, NULL));
    r = numbered(101); append_rejected(&h, &r);
    r = numbered(100); append_rejected(&h, &r);
    r = numbered(102); --r.utc_end_us; append_rejected(&h, &r);
    r = numbered(102); r.utc_end_us = numbered(100).utc_end_us; append_rejected(&h, &r);
    r = numbered(102); r.source = 1; append_rejected(&h, &r);
    r = numbered(102); r.sequence = UINT64_MAX;
    assert(rtc_history_append(&h, &r));
    r = numbered(103); append_rejected(&h, &r);
    rtc_history_init(&h);
    r = numbered(1); assert(rtc_history_append(&h, &r));
    r = numbered(2); --r.utc_end_us; r.epoch = 2;
    assert(rtc_history_append(&h, &r));
    r = numbered(3); r.utc_end_us -= r.utc_delta_us / 2; r.epoch = 1;
    assert(rtc_history_append(&h, &r));
    r = numbered(4); r.utc_end_us -= r.utc_delta_us; r.source = 0;
    ++r.utc_end_us;
    assert(rtc_history_append(&h, &r));
    r = numbered(5); r.utc_end_us = sample().utc_end_us + r.utc_delta_us * 13 / 4;
    append_rejected(&h, &r);
    r = numbered(6); r.source = 0;
    assert(rtc_history_append(&h, &r));
    h.generation = UINT64_MAX; r = numbered(7); append_rejected(&h, &r);
    rtc_history_init(&h); h.count = 101; append_rejected(&h, &r);
    assert(!rtc_history_get(&h, 0, &got));
    rtc_history_init(&h); h.next = 100; append_rejected(&h, &r);
    rtc_history_init(&h); h.count = 1; append_rejected(&h, &r);
}

static void pagination(void)
{
    rtc_history_t h;
    rtc_history_page_t page, before;
    rtc_history_init(&h);
    assert(rtc_history_page(&h, 0, 0, &page));
    assert(!page.count && !page.total && !page.cursor && !page.generation);
    for (uint64_t n = 1; n <= 137; ++n) {
        rtc_history_record_t r = numbered(n);
        assert(rtc_history_append(&h, &r));
        if (n == 1) {
            assert(rtc_history_page(&h, 1, 0, &page));
            assert(page.count == 1 && page.total == 1 && page.cursor == 1);
            assert(page.rows[0].sequence == 1);
        }
    }
    for (size_t cursor = 0; cursor <= 100; cursor += 5) {
        assert(rtc_history_page(&h, 137, cursor, &page));
        assert(page.total == 100 && page.generation == 137);
        assert(page.count == (cursor == 100 ? 0 : 5));
        assert(page.cursor == cursor + page.count);
        for (size_t i = 0; i < page.count; ++i)
            assert(page.rows[i].sequence == 38 + cursor + i);
    }
    assert(rtc_history_page(&h, 137, 98, &page));
    assert(page.count == 2 && page.cursor == 100);
    memcpy(&before, &page, sizeof before);
    assert(!rtc_history_page(&h, 136, 0, &page));
    assert(memcmp(&page, &before, sizeof page) == 0);
    assert(!rtc_history_page(&h, 137, 101, &page));
    assert(memcmp(&page, &before, sizeof page) == 0);
    assert(!rtc_history_page(&h, 137, SIZE_MAX, &page));
    assert(!rtc_history_page(NULL, 0, 0, &page));
    assert(!rtc_history_page(&h, 137, 0, NULL));
    rtc_history_record_t r = numbered(138);
    r.epoch = 999; r.raw.us = RTC_HISTORY_MAX_WINDOW_US;
    r.utc_delta_us = RTC_HISTORY_MAX_WINDOW_US;
    append_rejected(&h, &r);
    r = numbered(138);
    assert(rtc_history_append(&h, &r));
    assert(!rtc_history_page(&h, 137, 5, &page));
    assert(memcmp(&page, &before, sizeof page) == 0);
    assert(rtc_history_page(&h, 138, 0, &page));
    assert(page.rows[0].sequence == 39);
    h.count = 101;
    assert(!rtc_history_page(&h, 138, 0, &page));
}

int main(void)
{
    roundtrip();
    validation();
    codec();
    ring();
    pagination();
    puts("PASS rtc_history roundtrip, semantic admission, golden wire, 640 bit flips, 1000 property mutations, ring, pagination");
    printf("RAM bytes: record=%zu ring=%zu page=%zu reserved_stack=1024 total=%zu\n",
        sizeof(rtc_history_record_t), sizeof(rtc_history_t), sizeof(rtc_history_page_t),
        sizeof(rtc_history_t) + sizeof(rtc_history_page_t) + 1024);
    return 0;
}
