#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "auth_time_sync.h"

int main(void)
{
    auth_time_sync_cache_t cache = {0};
    auth_time_sync_reset(&cache, 0x12345678);
    auth_time_sync_probe_t p = auth_time_sync_record(&cache, 10000, 30000);
    assert(p.valid && p.id != 0 && p.received_us == 10000 && p.sent_us == 30000);
    auth_time_sync_reference_t r;
    /* 30 ms total - 20 ms handler = 10 ms transport, plus 1 ms quantization. */
    assert(auth_time_sync_resolve(&cache, p.id, 1700000000123456ULL,
        30001, 200, 40000, &r));
    assert(r.hrt_us == 20000 && r.utc_us == 1700000000123456ULL);
    assert(r.reference_id == (UINT64_C(0x12345678) << 32 | p.id));
    assert(r.transport_uncertainty_us == 6201 && r.phone_uncertainty_us == 1000000);
    assert(auth_time_sync_resolve(&cache, p.id, r.utc_us, 30001, 200, 5020000, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, r.utc_us, 30001, 200, 5020001, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, r.utc_us, 30001, 200, 19999, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, r.utc_us, 19999, 0, 40000, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, r.utc_us, 500001, 0, 40000, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, r.utc_us, 30001, 20001, 40000, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, 1577836799999999ULL, 30001, 0, 40000, &r));
    assert(!auth_time_sync_resolve(&cache, p.id, 4102444800000001ULL, 30001, 0, 40000, &r));
    assert(auth_time_sync_consume(&cache, p.id));
    assert(!auth_time_sync_consume(&cache, p.id));
    assert(!auth_time_sync_resolve(&cache, p.id, 1700000000000000ULL, 30001, 0, 40000, &r));

    p = auth_time_sync_record(&cache, 40000, 40020);
    for (unsigned i = 0; i < 15; ++i) auth_time_sync_record(&cache, 41000 + i, 41020 + i);
    assert(auth_time_sync_resolve(&cache, p.id, 1700000000000000ULL, 1000, 0, 50000, &r));
    auth_time_sync_record(&cache, 42000, 42020);
    assert(!auth_time_sync_resolve(&cache, p.id, 1700000000000000ULL, 1000, 0, 50000, &r));
    p = auth_time_sync_record(&cache, 50000, 50020);
    auth_time_sync_reset(&cache, 9);
    assert(!auth_time_sync_consume(&cache, p.id));
    auth_time_sync_probe_t next = auth_time_sync_record(&cache, 50000, 50020);
    assert(next.id > p.id);
    cache.counter = UINT32_MAX - 1;
    p = auth_time_sync_record(&cache, 60000, 60020);
    assert(p.id == UINT32_MAX);
    next = auth_time_sync_record(&cache, 61000, 61020);
    assert(next.id == 1 && !auth_time_sync_consume(&cache, p.id));
    assert(!auth_time_sync_record(&cache, -1, 20).valid);
    assert(!auth_time_sync_record(&cache, 30, 20).valid);
    assert(!auth_time_sync_record(&cache, 0, 500001).valid);
    assert(!auth_time_sync_record(&cache, INT64_MAX - 20, INT64_MAX).valid);
    assert(sizeof cache <= 800);
    puts("Precise time probes: subtraction, bounds, uncertainty, expiry, lifecycle and replay: PASS");
    return 0;
}
