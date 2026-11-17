#include "rtc_history_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[13][832];
    size_t size[13];
    int operation, fail, ack, corrupt;
} memory_t;
static int key_index(const char *key)
{
    if (key[0] == 'm') return 11 + key[1] - '0';
    return key[1] <= '9' ? key[1] - '0' : 10;
}
static bool fault(memory_t *m) { return ++m->operation == m->fail; }
static rtc_history_store_result_t rd(void *u, const char *key, void *p, size_t *n)
{
    memory_t *m = u;
    if (fault(m)) return RTC_HISTORY_STORE_IO_ERROR;
    int k = key_index(key);
    if (!m->size[k]) return RTC_HISTORY_STORE_MISSING;
    if (*n < m->size[k]) return RTC_HISTORY_STORE_BLOCKED;
    *n = m->size[k]; memcpy(p, m->data[k], *n);
    if (m->operation == m->corrupt) ((uint8_t *)p)[0] ^= 1;
    return RTC_HISTORY_STORE_VALID;
}
static bool wr(void *u, const char *key, const void *p, size_t n)
{
    memory_t *m = u;
    bool fail = fault(m);
    if (fail && !m->ack) return false;
    int k = key_index(key); assert(n <= 832);
    memcpy(m->data[k], p, n); m->size[k] = n;
    return !fail;
}
static bool commit(void *u) { return !fault(u); }
static rtc_history_store_io_t io(memory_t *m)
{
    return (rtc_history_store_io_t){m, rd, wr, commit};
}
static rtc_history_record_t sample(uint64_t n)
{
    return (rtc_history_record_t){.source = 2, .sequence = n, .epoch = 1,
        .utc_end_us = UINT64_C(1700000000000000) + n * RTC_HISTORY_MIN_RAW_US,
        .raw = {RTC_HISTORY_MIN_RAW_US + n, 0}, .utc_delta_us = RTC_HISTORY_MIN_RAW_US};
}
static void check(rtc_history_t *h, uint64_t n)
{
    assert(h->generation == n && h->count == (n > 100 ? 100 : n));
    for (size_t i = 0; i < h->count; ++i) {
        rtc_history_record_t got, want = sample(n - h->count + i + 1);
        uint8_t a[80], b[80];
        assert(rtc_history_get(h, i, &got));
        assert(rtc_history_encode(&got, a, 80) && rtc_history_encode(&want, b, 80));
        assert(!memcmp(a, b, 80));
    }
}
static void rotations(void)
{
    memory_t m = {0}; rtc_history_t h; rtc_history_store_t s;
    assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_MISSING);
    for (uint64_t n = 1; n <= 351; ++n) {
        rtc_history_record_t r = sample(n);
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_VALID);
        check(&h, n);
        assert(s.checkpoint.accepted_sequence == n && s.checkpoint.dropped == 0);
        int operations = m.operation;
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_VALID);
        assert(operations == m.operation);
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_VALID);
        check(&h, n);
        assert(s.checkpoint.accepted_sequence == n && s.checkpoint.dropped == 0);
    }
    rtc_history_record_t r = sample(352); --r.utc_end_us;
    assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_BLOCKED);
    check(&h, 351);
    assert(rtc_history_store_open(&s, &h, io(&m), 0) == RTC_HISTORY_STORE_BLOCKED);
}
static void faults(void)
{
    memory_t base = {0}; rtc_history_t h; rtc_history_store_t s;
    assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_MISSING);
    for (uint64_t n = 1; n <= 110; ++n) {
        rtc_history_record_t r = sample(n);
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_VALID);
    }
    for (int empty = 0; empty < 2; ++empty) for (int ack = 0; ack < 2; ++ack) {
        for (int boundary = 1; boundary <= 7; ++boundary) {
            memory_t m = empty ? (memory_t){0} : base;
            assert(rtc_history_store_open(&s, &h, io(&m), 2) ==
                (empty ? RTC_HISTORY_STORE_MISSING : RTC_HISTORY_STORE_VALID));
            m.operation = 0; m.fail = boundary; m.ack = ack;
            rtc_history_record_t r = sample(empty ? 1 : 111);
            rtc_history_store_result_t result = rtc_history_store_append(&s, &r, NULL);
            assert(result == RTC_HISTORY_STORE_IO_ERROR || result == RTC_HISTORY_STORE_VALID);
            if (result == RTC_HISTORY_STORE_IO_ERROR) check(&h, empty ? 0 : 110);
            m.fail = 0;
            result = rtc_history_store_open(&s, &h, io(&m), 2);
            assert(result == RTC_HISTORY_STORE_VALID || result == RTC_HISTORY_STORE_MISSING);
            uint64_t n = h.generation;
            assert(n == (empty ? 0u : 110u) || n == (empty ? 1u : 111u));
            check(&h, n);
        }
    }
    for (int k = 0; k < 13; ++k) if (base.size[k]) {
        memory_t m = base; m.data[k][0] ^= 128;
        rtc_history_store_result_t result = rtc_history_store_open(&s, &h, io(&m), 2);
        assert(result == RTC_HISTORY_STORE_BLOCKED || result == RTC_HISTORY_STORE_VALID);
        if (result == RTC_HISTORY_STORE_VALID) check(&h, 110);
    }
    memory_t m = {0}; m.size[0] = 832; m.data[0][0] = 99;
    assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
}
static void fix_crc(uint8_t *p, size_t size, size_t offset)
{
    uint32_t crc = UINT32_MAX;
    memset(p + offset, 0, 4);
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0 - (crc & 1)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) p[offset + i] = (uint8_t)(crc >> (8 * i));
}
static void incompatible(void)
{
    memory_t base = {0}; rtc_history_t h; rtc_history_store_t s;
    assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_MISSING);
    for (uint64_t n = 1; n <= 101; ++n) {
        rtc_history_record_t r = sample(n);
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_VALID);
    }
    const unsigned fields[] = {0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 52, 56, 57, 66};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        memory_t m = base;
        int manifest = 11 + s.manifest;
        if (fields[i] == 40) memset(m.data[manifest] + 40, 255, 8);
        else m.data[manifest][fields[i]] ^= 128;
        fix_crc(m.data[manifest], 256, 252);
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
        rtc_history_record_t r = sample(102);
        int operations = m.operation;
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_BLOCKED);
        assert(operations == m.operation);
        assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_VALID);
    }
    unsigned slot = s.map[0];
    const unsigned chunk_fields[] = {0, 4, 8, 12, 16, 20, 24};
    for (size_t i = 0; i < sizeof chunk_fields / sizeof chunk_fields[0]; ++i) {
        memory_t m = base;
        m.data[slot][chunk_fields[i]] ^= 128;
        fix_crc(m.data[slot], 832, 28);
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
    }
    memory_t m = base;
    memset(m.data[slot] + 32 + 8, 0, 8);
    fix_crc(m.data[slot] + 32, 80, 76); fix_crc(m.data[slot], 832, 28);
    assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
    m = base;
    rtc_history_record_t row = sample(101);
    assert(rtc_history_encode(&row, m.data[slot] + 32 + 80, 80));
    fix_crc(m.data[slot], 832, 28);
    assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
    for (int boundary = 1; boundary <= 25; ++boundary) {
        m = base; m.operation = 0; m.fail = boundary;
        rtc_history_store_result_t result = rtc_history_store_open(&s, &h, io(&m), 2);
        assert(result == RTC_HISTORY_STORE_IO_ERROR || result == RTC_HISTORY_STORE_VALID);
        if (result == RTC_HISTORY_STORE_VALID) check(&h, 101);
    }
    for (int boundary = 4; boundary <= 7; boundary += 3) {
        m = base;
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_VALID);
        m.operation = 0; m.corrupt = boundary;
        uint8_t map[10]; memcpy(map, s.map, 10);
        rtc_history_record_t r = sample(102);
        assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_IO_ERROR);
        check(&h, 101);
        for (unsigned j = 0; j < 10; ++j)
            assert(!memcmp(m.data[map[j]], base.data[map[j]], 832));
        m.corrupt = 0;
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_VALID);
        check(&h, boundary == 4 ? 101 : 102);
    }
}
static void checkpoints(void)
{
    memory_t base = {0}; rtc_history_t h; rtc_history_store_t s;
    assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_MISSING);
    assert(s.checkpoint.accepted_sequence == 0 && s.checkpoint.dropped == 0);
    rtc_history_record_t r = sample(1), last;
    rtc_history_store_checkpoint_t cp = {4, 2};
    base.operation = 0;
    assert(rtc_history_store_append(&s, &r, &cp) == RTC_HISTORY_STORE_VALID);
    assert(base.operation == 6);
    assert(s.checkpoint.accepted_sequence == 4 && s.checkpoint.dropped == 2);
    assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_VALID);
    assert(s.checkpoint.accepted_sequence == 4 && s.checkpoint.dropped == 2);
    assert(rtc_history_get(&h, 0, &last) && last.sequence == 1 && h.generation == 1);
    int operations = base.operation;
    rtc_history_store_checkpoint_t newer = {6, 3};
    assert(rtc_history_store_append(&s, &r, &newer) == RTC_HISTORY_STORE_VALID);
    assert(base.operation == operations && h.generation == 1);
    assert(s.checkpoint.accepted_sequence == 4 && s.checkpoint.dropped == 2);
    r = sample(2);
    const rtc_history_store_checkpoint_t invalid[] = {{3, 2}, {4, 1}, {4, 5}, {1, 0}};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        rtc_history_t before = h;
        assert(rtc_history_store_append(&s, &r, &invalid[i]) == RTC_HISTORY_STORE_BLOCKED);
        assert(base.operation == operations && !memcmp(&h, &before, sizeof h));
        assert(s.checkpoint.accepted_sequence == 4 && s.checkpoint.dropped == 2);
    }
    assert(rtc_history_store_append(&s, &r, &cp) == RTC_HISTORY_STORE_VALID);
    assert(h.generation == 2 && s.checkpoint.accepted_sequence == 4);
    r = sample(7);
    assert(rtc_history_store_append(&s, &r, NULL) == RTC_HISTORY_STORE_VALID);
    assert(s.checkpoint.accepted_sequence == 7 && s.checkpoint.dropped == 2);
    r = sample(8);
    for (int ack = 0; ack < 2; ++ack) for (int boundary = 1; boundary <= 7; ++boundary) {
        memory_t m = base;
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_VALID);
        m.operation = 0; m.fail = boundary; m.ack = ack;
        rtc_history_store_checkpoint_t next = {10, 3};
        rtc_history_store_result_t result = rtc_history_store_append(&s, &r, &next);
        assert(result == RTC_HISTORY_STORE_IO_ERROR || result == RTC_HISTORY_STORE_VALID);
        if (result == RTC_HISTORY_STORE_IO_ERROR) {
            assert(h.generation == 3 && s.checkpoint.accepted_sequence == 7 && s.checkpoint.dropped == 2);
        }
        m.fail = 0;
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_VALID);
        assert(rtc_history_get(&h, h.count - 1, &last));
        bool added = h.generation == 4;
        assert(last.sequence == (added ? 8u : 7u));
        assert(s.checkpoint.accepted_sequence == (added ? 10u : 7u));
        assert(s.checkpoint.dropped == (added ? 3u : 2u));
        if (added) {
            operations = m.operation;
            assert(rtc_history_store_append(&s, &r, &newer) == RTC_HISTORY_STORE_VALID);
            assert(m.operation == operations && h.generation == 4);
            assert(s.checkpoint.accepted_sequence == 10 && s.checkpoint.dropped == 3);
        }
    }
    assert(rtc_history_store_open(&s, &h, io(&base), 2) == RTC_HISTORY_STORE_VALID);
    unsigned manifest = 11 + s.manifest;
    const unsigned offsets[] = {72, 80, 66, 71, 88, 251, 4};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        memory_t m = base;
        if (offsets[i] == 72) memset(m.data[manifest] + 72, 0, 8);
        else if (offsets[i] == 80) memset(m.data[manifest] + 80, 255, 8);
        else m.data[manifest][offsets[i]] = 1;
        fix_crc(m.data[manifest], 256, 252);
        uint8_t before[13][832]; memcpy(before, m.data, sizeof before);
        assert(rtc_history_store_open(&s, &h, io(&m), 2) == RTC_HISTORY_STORE_BLOCKED);
        assert(!memcmp(before, m.data, sizeof before));
    }
}
int main(void)
{
    rotations(); faults(); incompatible(); checkpoints();
    printf("PASS store rotations, reopen, dedup, semantic rejection, incompatibility, protocol faults; RAM=%zu\n",
        sizeof(rtc_history_store_t) + sizeof(rtc_history_t) + sizeof(rtc_history_page_t));
}
