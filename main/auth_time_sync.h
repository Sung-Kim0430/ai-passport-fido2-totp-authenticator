#ifndef AUTH_TIME_SYNC_H
#define AUTH_TIME_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AUTH_TIME_SYNC_PROBES 16
#define AUTH_TIME_SYNC_PHONE_UNCERTAINTY_US UINT64_C(250000)

/* HTTP-task-owned, independent of the legacy status-probe cache. */
typedef struct {
    uint32_t id;
    int64_t received_us;
    int64_t sent_us;
    bool valid;
} auth_time_sync_probe_t;

typedef struct {
    auth_time_sync_probe_t probes[AUTH_TIME_SYNC_PROBES];
    uint32_t counter;
    uint32_t nonce;
} auth_time_sync_cache_t;

typedef struct {
    uint64_t reference_id;
    uint64_t utc_us;
    int64_t hrt_us;
    uint64_t transport_uncertainty_us;
    uint64_t phone_uncertainty_us;
} auth_time_sync_reference_t;

static inline void auth_time_sync_reset(auth_time_sync_cache_t *cache, uint32_t nonce)
{
    memset(cache->probes, 0, sizeof cache->probes);
    cache->nonce = nonce;
}

static inline auth_time_sync_probe_t auth_time_sync_record(auth_time_sync_cache_t *cache,
    int64_t received_us, int64_t sent_us)
{
    if (++cache->counter == 0) {
        auth_time_sync_reset(cache, cache->nonce + 1);
        ++cache->counter;
    }
    const bool valid = received_us >= 0 && sent_us >= received_us &&
        sent_us <= INT64_C(9007199254740991) && sent_us - received_us <= 500000;
    auth_time_sync_probe_t p = {
        .id = cache->counter, .received_us = valid ? received_us : 0,
        .sent_us = valid ? sent_us : 0, .valid = valid,
    };
    cache->probes[(p.id - 1) % AUTH_TIME_SYNC_PROBES] = p;
    return p;
}

static inline bool auth_time_sync_resolve(const auth_time_sync_cache_t *cache,
    uint32_t id, uint64_t utc_us, uint64_t rtt_us, uint64_t spread_us,
    int64_t now_us, auth_time_sync_reference_t *out)
{
    if (!id || utc_us < UINT64_C(1577836800000000) || utc_us > UINT64_C(4102444800000000) ||
        !rtt_us || rtt_us > 500000 || spread_us > 20000) return false;
    const auth_time_sync_probe_t *p = &cache->probes[(id - 1) % AUTH_TIME_SYNC_PROBES];
    if (!p->valid || p->id != id) return false;
    const uint64_t processing_us = (uint64_t)(p->sent_us - p->received_us);
    const int64_t midpoint = p->received_us + (int64_t)(processing_us / 2);
    if (rtt_us < processing_us || now_us < midpoint || now_us - midpoint > 5000000) return false;
    const uint64_t network_us = rtt_us - processing_us;
    *out = (auth_time_sync_reference_t){
        .reference_id = ((uint64_t)cache->nonce << 32) | id,
        .utc_us = utc_us, .hrt_us = midpoint,
        /* A conservative asymmetry bound, not a claim of measured UTC accuracy. */
        .transport_uncertainty_us = (network_us + 1) / 2 + 1000 + spread_us,
        .phone_uncertainty_us = AUTH_TIME_SYNC_PHONE_UNCERTAINTY_US,
    };
    return true;
}

static inline bool auth_time_sync_consume(auth_time_sync_cache_t *cache, uint32_t id)
{
    if (!id) return false;
    auth_time_sync_probe_t *p = &cache->probes[(id - 1) % AUTH_TIME_SYNC_PROBES];
    if (!p->valid || p->id != id) return false;
    p->valid = false;
    return true;
}

#endif
