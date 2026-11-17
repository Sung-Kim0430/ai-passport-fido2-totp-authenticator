#include "rtc_history_store.h"

#include <string.h>

static void put(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t crc(const uint8_t *p, size_t n, size_t skip)
{
    uint32_t v = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        v ^= i >= skip && i < skip + 4 ? 0 : p[i];
        for (unsigned b = 0; b < 8; ++b)
            v = (v >> 1) ^ (UINT32_C(0xedb88320) & (0 - (v & 1)));
    }
    return ~v;
}
static void key(char out[3], char type, unsigned slot)
{
    out[0] = type; out[1] = "0123456789a"[slot]; out[2] = 0;
}
static void header(uint8_t *p, char type, uint32_t source)
{
    memcpy(p, type == 'm' ? "RHM1" : "RHC1", 4);
    put(p + 4, type == 'm' ? 2 : 1, 4); put(p + 8, source, 4);
    put(p + 12, 40, 4); put(p + 16, 1, 4);
}
static bool compatible(const uint8_t *p, char type, uint32_t source)
{
    return !memcmp(p, type == 'm' ? "RHM1" : "RHC1", 4) &&
        get(p + 4, 4) == (type == 'm' ? 2u : 1u) && get(p + 8, 4) == source &&
        get(p + 12, 4) == 40 && get(p + 16, 4) == 1;
}
static rtc_history_store_result_t read_blob(rtc_history_store_t *s, char type,
    unsigned slot, void *p, size_t expected)
{
    char name[3]; key(name, type, slot);
    size_t size = expected;
    rtc_history_store_result_t r = s->io.read(s->io.user, name, p, &size);
    if (r == RTC_HISTORY_STORE_VALID && size != expected) return RTC_HISTORY_STORE_BLOCKED;
    return r;
}
static bool chunk_valid(rtc_history_store_t *s)
{
    uint8_t *p = s->scratch;
    if (!compatible(p, 'c', s->source) || get(p + 20, 4) >= 10 ||
        !get(p + 24, 4) || get(p + 24, 4) > 10 || get(p + 28, 4) != crc(p, 832, 28)) return false;
    for (unsigned i = 0; i < get(p + 24, 4); ++i) {
        rtc_history_record_t row;
        if (!rtc_history_decode(p + 32 + i * 80, 80, &row) || row.source != s->source) return false;
    }
    for (size_t i = 32 + (size_t)get(p + 24, 4) * 80; i < 832; ++i)
        if (p[i]) return false;
    return true;
}
static bool manifest_valid(rtc_history_store_t *s, const uint8_t *p)
{
    uint64_t generation = get(p + 24, 8);
    unsigned count = (unsigned)get(p + 48, 4), head = (unsigned)get(p + 52, 4);
    if (!compatible(p, 'm', s->source) || get(p + 20, 4) != 1 ||
        get(p + 252, 4) != crc(p, 256, 252) || !generation ||
        count != (generation < 100 ? generation : 100) || head != generation % 100 ||
        !get(p + 32, 8) || get(p + 72, 8) < get(p + 32, 8) ||
        get(p + 80, 8) > get(p + 72, 8) || (generation <= 100 && get(p + 40, 8))) return false;
    unsigned mask = 0;
    for (unsigned i = 0; i < 10; ++i) {
        unsigned slot = p[56 + i];
        if (i * 10 >= count && count < 100) {
            if (slot != 255) return false;
        } else {
            if (slot > 10 || (mask & (1u << slot))) return false;
            mask |= 1u << slot;
        }
    }
    for (unsigned i = 66; i < 72; ++i) if (p[i]) return false;
    for (unsigned i = 88; i < 252; ++i) if (p[i]) return false;
    return true;
}
rtc_history_store_result_t rtc_history_store_open(rtc_history_store_t *s,
    rtc_history_t *h, rtc_history_store_io_t io, uint32_t source)
{
    if (!s || !h) return RTC_HISTORY_STORE_BLOCKED;
    memset(s, 0, sizeof *s); rtc_history_init(h);
    s->io = io; s->history = h; s->source = source;
    s->status = RTC_HISTORY_STORE_BLOCKED; memset(s->map, 255, 10);
    if (!io.read || !io.write || !io.commit || (source != 0 && source != 2)) return s->status;
    uint8_t selected[256] = {0}, candidate[256];
    bool found = false;
    for (unsigned i = 0; i < 2; ++i) {
        rtc_history_store_result_t r = read_blob(s, 'm', i, candidate, 256);
        if (r == RTC_HISTORY_STORE_MISSING) continue;
        if (r != RTC_HISTORY_STORE_VALID) { s->status = r; return r; }
        if (!manifest_valid(s, candidate)) return s->status;
        if (found && get(selected + 24, 8) == get(candidate + 24, 8) &&
            memcmp(selected, candidate, 256)) return s->status;
        if (!found || get(candidate + 24, 8) > get(selected + 24, 8)) {
            memcpy(selected, candidate, 256); s->manifest = (uint8_t)i; found = true;
        }
    }
    for (unsigned i = 0; i < 11; ++i) {
        rtc_history_store_result_t r = read_blob(s, 'c', i, s->scratch, 832);
        if (r == RTC_HISTORY_STORE_MISSING) continue;
        if (r != RTC_HISTORY_STORE_VALID) { s->status = r; return r; }
        if (!chunk_valid(s)) return s->status;
    }
    if (!found) { s->status = RTC_HISTORY_STORE_MISSING; return s->status; }
    unsigned count = (unsigned)get(selected + 48, 4), head = (unsigned)get(selected + 52, 4);
    uint64_t watermark = get(selected + 40, 8);
    if (count == 100 && (watermark < RTC_HISTORY_UTC_MIN_US || watermark > RTC_HISTORY_UTC_MAX_US) &&
        get(selected + 24, 8) > 100) return s->status;
    h->evicted_utc_end_us = watermark;
    unsigned loaded = 100;
    for (unsigned i = 0; i < count; ++i) {
        unsigned index = (head + 100 - count + i) % 100, logical = index / 10;
        if (loaded != logical) {
            rtc_history_store_result_t r = read_blob(s, 'c', selected[56 + logical], s->scratch, 832);
            if (r != RTC_HISTORY_STORE_VALID || !chunk_valid(s) ||
                get(s->scratch + 20, 4) != logical) {
                rtc_history_init(h);
                s->status = r == RTC_HISTORY_STORE_IO_ERROR ? r : RTC_HISTORY_STORE_BLOCKED;
                return s->status;
            }
            loaded = logical;
        }
        rtc_history_record_t row;
        if (index % 10 >= get(s->scratch + 24, 4) ||
            !rtc_history_decode(s->scratch + 32 + (index % 10) * 80, 80, &row) ||
            !rtc_history_append(h, &row)) { rtc_history_init(h); return s->status; }
    }
    rtc_history_record_t last;
    if (!rtc_history_get(h, count - 1, &last) || last.sequence != get(selected + 32, 8)) {
        rtc_history_init(h); return s->status;
    }
    if (count == 100 && head) {
        rtc_history_record_t temp[10];
        for (unsigned moved = 0; moved < head; moved += 10) {
            unsigned amount = head - moved < 10 ? head - moved : 10;
            memcpy(temp, h->rows + 100 - amount, amount * sizeof *temp);
            memmove(h->rows + amount, h->rows, (100 - amount) * sizeof *temp);
            memcpy(h->rows, temp, amount * sizeof *temp);
        }
    }
    h->next = head; h->generation = get(selected + 24, 8);
    s->checkpoint.accepted_sequence = get(selected + 72, 8);
    s->checkpoint.dropped = get(selected + 80, 8);
    memcpy(s->map, selected + 56, 10);
    s->status = RTC_HISTORY_STORE_VALID;
    return s->status;
}
static bool persist(rtc_history_store_t *s, char type, unsigned slot, size_t size)
{
    char name[3]; key(name, type, slot);
    uint32_t expected = crc(s->scratch, size, size);
    return s->io.write(s->io.user, name, s->scratch, size) && s->io.commit(s->io.user) &&
        read_blob(s, type, slot, s->scratch, size) == RTC_HISTORY_STORE_VALID &&
        crc(s->scratch, size, size) == expected;
}
rtc_history_store_result_t rtc_history_store_append(rtc_history_store_t *s,
    const rtc_history_record_t *r, const rtc_history_store_checkpoint_t *checkpoint)
{
    if (!s || !r || (s->status != RTC_HISTORY_STORE_VALID && s->status != RTC_HISTORY_STORE_MISSING) ||
        r->source != s->source) return RTC_HISTORY_STORE_BLOCKED;
    rtc_history_t *h = s->history;
    uint8_t wire[80], prior[80];
    if (!rtc_history_encode(r, wire, 80)) return RTC_HISTORY_STORE_BLOCKED;
    for (size_t i = 0; i < h->count; ++i) {
        rtc_history_record_t row;
        if (!rtc_history_get(h, i, &row)) return RTC_HISTORY_STORE_BLOCKED;
        if (row.sequence == r->sequence && row.epoch == r->epoch) {
            if (!rtc_history_encode(&row, prior, 80) || memcmp(wire, prior, 80)) return RTC_HISTORY_STORE_BLOCKED;
            return RTC_HISTORY_STORE_VALID;
        }
    }
    rtc_history_store_checkpoint_t next = checkpoint ? *checkpoint : s->checkpoint;
    if (!checkpoint && next.accepted_sequence < r->sequence) next.accepted_sequence = r->sequence;
    if (next.accepted_sequence < r->sequence ||
        next.accepted_sequence < s->checkpoint.accepted_sequence ||
        next.dropped < s->checkpoint.dropped || next.dropped > next.accepted_sequence)
        return RTC_HISTORY_STORE_BLOCKED;
    size_t index = h->next, count = h->count;
    uint64_t generation = h->generation, watermark = h->evicted_utc_end_us;
    rtc_history_record_t saved = h->rows[index];
    if (!rtc_history_append(h, r)) return RTC_HISTORY_STORE_BLOCKED;
    uint8_t manifest[256] = {0};
    header(manifest, 'm', s->source); put(manifest + 20, 1, 4);
    put(manifest + 24, h->generation, 8); put(manifest + 32, r->sequence, 8);
    put(manifest + 40, h->evicted_utc_end_us, 8); put(manifest + 48, h->count, 4);
    put(manifest + 52, h->next, 4); memcpy(manifest + 56, s->map, 10);
    put(manifest + 72, next.accepted_sequence, 8); put(manifest + 80, next.dropped, 8);
    h->rows[index] = saved; h->count = count; h->next = index;
    h->generation = generation; h->evicted_utc_end_us = watermark;
    unsigned logical = (unsigned)index / 10, spare = 0;
    for (; spare < 11; ++spare) {
        bool used = false;
        for (unsigned j = 0; j < 10; ++j) if (s->map[j] == spare) used = true;
        if (!used) break;
    }
    if (s->map[logical] != 255) {
        if (read_blob(s, 'c', s->map[logical], s->scratch, 832) != RTC_HISTORY_STORE_VALID ||
            !chunk_valid(s) || get(s->scratch + 20, 4) != logical) goto io_error;
    } else {
        memset(s->scratch, 0, 832); header(s->scratch, 'c', s->source);
        put(s->scratch + 20, logical, 4);
    }
    unsigned rows = (unsigned)get(s->scratch + 24, 4);
    if (rows < index % 10 + 1) rows = (unsigned)(index % 10 + 1);
    put(s->scratch + 24, rows, 4); memcpy(s->scratch + 32 + (index % 10) * 80, wire, 80);
    put(s->scratch + 28, crc(s->scratch, 832, 28), 4);
    if (!persist(s, 'c', spare, 832)) goto io_error;
    manifest[56 + logical] = (uint8_t)spare;
    put(manifest + 252, crc(manifest, 256, 252), 4);
    memcpy(s->scratch, manifest, 256);
    unsigned alternate = s->manifest ^ 1u;
    if (!persist(s, 'm', alternate, 256)) goto io_error;
    if (!rtc_history_append(h, r)) goto io_error;
    s->checkpoint = next;
    memcpy(s->map, manifest + 56, 10); s->manifest = (uint8_t)alternate;
    s->status = RTC_HISTORY_STORE_VALID;
    return s->status;
io_error:
    s->status = RTC_HISTORY_STORE_IO_ERROR;
    return s->status;
}
_Static_assert(sizeof(rtc_history_store_t) + sizeof(rtc_history_t) +
    sizeof(rtc_history_page_t) + 2048 <= 16384, "store RAM budget");
