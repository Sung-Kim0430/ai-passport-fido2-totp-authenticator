#include "rtc_clock_internal.h"
#include "rtc_light_sleep.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CLOCK_RTC_ATTR RTC_IRAM_ATTR
#else
#define CLOCK_RTC_ATTR
#endif

typedef struct {
    rtc_history_t ring;
    rtc_history_store_t store;
    rtc_history_estimate_workspace_t workspace;
} clock_history_cache_t;

_Static_assert(sizeof(clock_history_cache_t) + sizeof(rtc_clock_history_summary_t) +
    sizeof(rtc_history_page_t) + 2048 <= 16384, "normal history budget");

/* 普通任务单一所有；硬件 I/O 独立，以便运行同一状态机的主机回归。 */
static struct {
    clock_history_cache_t *history;
    bool history_started;
    bool history_retained_valid;
    bool history_busy;
    int64_t history_retry_us;
    rtc_history_store_result_t history_status;
    rtc_history_store_result_t history_save_result;
    rtc_history_estimate_t history_shadow;
    uint16_t battery_mv;
    bool battery_valid;
    int64_t battery_stamp_us;
    bool initialized;
    bool supported;
    bool boot_done;
    bool restore_authorized;
    bool save_blocked;
    bool saved_valid;
    rtc_clock_boot_result_t boot_result;
    rtc_drift_model_t saved_model;
    rtc_drift_direct_t direct;
    rtc_drift_temperature_t temperature;
    int64_t next_sample_us;
    uint32_t prepared_period;
    uint64_t reconciled_elapsed_us;
    int64_t sdk_correction_us;
    bool sdk_sample_valid;
    uint32_t source;
    bool continuity_valid;
    bool execution_valid;
    int64_t execution_hrt_us;
    int64_t execution_start_us;
    int64_t budget_hrt_us;
    bool coefficient_saved;
    bool coefficient_blocked;
    int32_t saved_ppm;
    bool maintenance;
    bool provisional;
    rtc_clock_session_t backup_session;
    uint32_t backup_maintenance_count;
    uint64_t corrected_elapsed_us;
    bool light_enabled;
    bool light_fault;
    bool light_needs_sync;
    rtc_light_sleep_snapshot_t light_seen;
} s_clock;

/* 位运算校验不依赖查表、日志或 libc；stub 也要在最终 ELF 中验证其放置。 */
uint32_t CLOCK_RTC_ATTR rtc_clock_checksum(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void CLOCK_RTC_ATTR rtc_clock_session_seal(rtc_clock_session_t *session)
{
    session->checksum = rtc_clock_checksum(session, offsetof(rtc_clock_session_t, checksum));
}

bool CLOCK_RTC_ATTR rtc_clock_session_valid(const rtc_clock_session_t *session)
{
    if (!session) return false;
    rtc_drift_ledger_t ledger = session->ledger;
    if (ledger.phase == RTC_DRIFT_LEDGER_CONSUMED) ledger.phase = RTC_DRIFT_LEDGER_ACTIVE;
    return session->magic == RTC_CLOCK_MAGIC && session->version == RTC_CLOCK_VERSION &&
           session->utc_trusted <= 1 && session->user_wake <= 1 && session->maintenance_enabled <= 1 &&
           (session->source == 0 || session->source == 2) && session->continuity &&
           session->ppm >= -20000 && session->ppm <= 20000 &&
           session->anchor_phase_q19 < RTC_DRIFT_Q19 && rtc_drift_ledger_valid(&ledger) &&
           session->checksum == rtc_clock_checksum(session, offsetof(rtc_clock_session_t, checksum));
}

static void history_seal(void)
{
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    h->checksum = rtc_clock_checksum(h, offsetof(rtc_clock_history_retained_t, checksum));
}

static bool history_retained_valid(void)
{
    const rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    if (h->checksum != rtc_clock_checksum(h, offsetof(rtc_clock_history_retained_t, checksum))) return false;
    if (h->version != 1 || !h->epoch || h->count > 2 || (h->flags & ~7u) ||
        h->budget_us > RTC_CLOCK_HISTORY_INTERVAL_US || h->durable_sequence > h->accepted_sequence ||
        h->dropped > h->accepted_sequence) return false;
    uint64_t previous = h->durable_sequence;
    for (uint32_t i = 0; i < h->count; ++i) {
        rtc_history_record_t row;
        if (!rtc_history_decode(h->rows[i], 80, &row) || row.source != s_clock.source ||
            row.epoch != h->epoch || row.sequence <= previous || row.sequence > h->accepted_sequence) return false;
        previous = row.sequence;
    }
    return true;
}

static void history_budget(uint64_t delta)
{
    if (!s_clock.history_retained_valid) return;
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    const uint64_t left = RTC_CLOCK_HISTORY_INTERVAL_US - h->budget_us;
    h->budget_us += delta < left ? delta : left;
    history_seal();
}

static void history_ack(void)
{
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    if (h->count == 2) memcpy(h->rows[0], h->rows[1], 80);
    if (h->count) --h->count;
    memset(h->rows[h->count], 0, 80);
    history_seal();
}

static bool history_reconcile(void)
{
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    rtc_history_t *ring = &s_clock.history->ring;
    const rtc_history_store_checkpoint_t checkpoint = s_clock.history->store.checkpoint;
    if (h->flags & 4u) {
        rtc_clock_history_retained_t rebased = *h;
        if (h->accepted_sequence > UINT64_MAX - checkpoint.accepted_sequence ||
            h->dropped > UINT64_MAX - checkpoint.dropped) return false;
        for (uint32_t i = 0; i < h->count; ++i) {
            rtc_history_record_t pending;
            if (!rtc_history_decode(h->rows[i], 80, &pending) ||
                pending.sequence > UINT64_MAX - checkpoint.accepted_sequence) return false;
            pending.sequence += checkpoint.accepted_sequence;
            if (!rtc_history_encode(&pending, rebased.rows[i], 80)) return false;
        }
        rebased.accepted_sequence += checkpoint.accepted_sequence;
        rebased.dropped += checkpoint.dropped;
        rebased.flags &= ~4u;
        *h = rebased;
    }
    rtc_history_record_t last;
    if (ring->count && rtc_history_get(ring, ring->count - 1, &last)) {
        if (last.sequence > h->accepted_sequence) h->accepted_sequence = last.sequence;
        h->durable_sequence = last.sequence;
    }
    if (checkpoint.accepted_sequence > h->accepted_sequence) h->accepted_sequence = checkpoint.accepted_sequence;
    if (checkpoint.dropped > h->dropped) h->dropped = checkpoint.dropped;
    h->generation = ring->generation;
    while (h->count) {
        rtc_history_record_t pending;
        if (!rtc_history_decode(h->rows[0], 80, &pending)) return false;
        if (pending.sequence > h->durable_sequence) break;
        bool found = false;
        for (size_t i = 0; i < ring->count; ++i) {
            rtc_history_record_t row;
            uint8_t wire[80];
            if (rtc_history_get(ring, i, &row) && row.sequence == pending.sequence &&
                rtc_history_encode(&row, wire, 80) && !memcmp(wire, h->rows[0], 80)) found = true;
        }
        if (!found) return false;
        history_ack();
    }
    history_seal();
    return true;
}

static void seal_accounting(void);

static void history_refresh(void)
{
    rtc_history_estimate_t estimate = {0};
    int64_t now;
    if (s_clock.history && (s_clock.history_status == RTC_HISTORY_STORE_VALID ||
            s_clock.history_status == RTC_HISTORY_STORE_MISSING) &&
        rtc_clock_platform_get_wall_us(&now) && now >= 0) {
        rtc_history_estimate(&s_clock.history->ring, s_clock.source, (uint64_t)now,
            rtc_clock_retained.accounting.active_ppm, &s_clock.history->workspace, &estimate);
    }
    s_clock.history_shadow = estimate;
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    const bool stable = estimate.result == RTC_HISTORY_ESTIMATE_READY && estimate.inliers >= 2;
    const bool recovery = estimate.result == RTC_HISTORY_ESTIMATE_RECOVERY &&
        a->active_qualified && estimate.holdout_present && estimate.holdout_improved;
    const bool recent = estimate.result == RTC_HISTORY_ESTIMATE_RECENT_READY &&
        a->active_qualified && estimate.holdout_present && estimate.holdout_improved;
    if (s_clock.boot_done && s_clock.supported && !rtc_clock_retained.history.count &&
        rtc_clock_retained.history.accepted_sequence == rtc_clock_retained.history.durable_sequence &&
        rtc_clock_platform_source() == s_clock.source &&
        (stable || recovery || recent) && isfinite(estimate.ppm) &&
        estimate.evidence_sequence > a->calibration.evidence_sequence &&
        estimate.evidence_utc_us >= a->calibration.evidence_utc_us) {
        const long long rounded = llround(estimate.ppm);
        if (rounded >= -20000 && rounded <= 20000) {
            const int32_t candidate = (int32_t)rounded;
            const bool initial = stable && !a->active_qualified && estimate.uncertainty_ppm <= 100.0;
            const bool refine = a->active_qualified && estimate.holdout_improved && candidate != a->active_ppm;
            if (initial || refine) {
                double uncertainty = estimate.uncertainty_ppm + fabs(estimate.ppm - candidate);
                for (size_t index = 0; index < s_clock.history->ring.count; ++index) {
                    rtc_history_record_t row;
                    double rate;
                    if (rtc_history_get(&s_clock.history->ring, index, &row) &&
                        row.sequence == estimate.evidence_sequence && rtc_history_rate_ppm(&row, &rate)) {
                        const double allowance = (double)row.combined_uncertainty_us * 1000000.0 /
                            (double)(row.utc_delta_us - row.awake_delta_us);
                        uncertainty = fmax(uncertainty, allowance + fabs(rate - candidate));
                    }
                }
                const double milli = ceil(uncertainty * 1000.0);
                if (!isfinite(milli) || milli < 0 || milli > UINT32_MAX) return;
                a->active_ppm = candidate;
                a->active_qualified = 1;
                a->calibration = (rtc_clock_calibration_t){
                    .quality = recovery ? RTC_CLOCK_CALIBRATION_RECOVERY : RTC_CLOCK_CALIBRATION_STABLE,
                    .estimator_version = estimate.version,
                    .evidence_sequence = estimate.evidence_sequence,
                    .evidence_utc_us = estimate.evidence_utc_us,
                    .uncertainty_millippm = (uint32_t)milli, .uncertainty_valid = 1
                };
                a->learning.ppm = candidate;
                a->learning.qualified = true;
                seal_accounting();
            }
        }
    }
}

static void history_open(void)
{
    rtc_history_store_t *store = &s_clock.history->store;
    const int64_t now = rtc_clock_platform_uptime_us();
    s_clock.history_retry_us = now > INT64_MAX - INT64_C(60000000) ? INT64_MAX : now + INT64_C(60000000);
    s_clock.history_busy = true;
    rtc_history_store_io_t io = store->io;
    if (!io.read || !io.write || !io.commit) {
        s_clock.history_status = rtc_history_store_platform_io(&io);
        if (s_clock.history_status == RTC_HISTORY_STORE_BLOCKED || s_clock.history_status == RTC_HISTORY_STORE_IO_ERROR) {
            s_clock.history_busy = false;
            history_refresh();
            return;
        }
    }
    s_clock.history_status = rtc_history_store_open(store, &s_clock.history->ring, io, s_clock.source);
    if ((s_clock.history_status == RTC_HISTORY_STORE_VALID || s_clock.history_status == RTC_HISTORY_STORE_MISSING) &&
        !history_reconcile()) s_clock.history_status = RTC_HISTORY_STORE_BLOCKED;
    s_clock.history_busy = false;
    history_refresh();
}

static void reset_learning(void);

static void history_start(void)
{
    if (s_clock.history_started || !s_clock.initialized || s_clock.maintenance) return;
    s_clock.history_started = true;
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    const bool fresh = !s_clock.history_retained_valid;
    if (fresh) {
        if (s_clock.continuity_valid) reset_learning();
        memset(h, 0, sizeof *h);
        h->version = 1;
        h->flags = 5;
        h->epoch = rtc_clock_platform_history_epoch();
        if (!h->epoch) h->epoch = 1;
        s_clock.history_retained_valid = true;
        history_seal();
    }
    s_clock.history_status = RTC_HISTORY_STORE_BLOCKED;
    s_clock.history = calloc(1, sizeof *s_clock.history);
    if (!s_clock.history) return;
    history_open();
}

static void history_checkpoint(void)
{
    if (!s_clock.history_started || !s_clock.history || s_clock.history_busy ||
        s_clock.history_status == RTC_HISTORY_STORE_BLOCKED) return;
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    if (!h->count || h->budget_us < RTC_CLOCK_HISTORY_INTERVAL_US) return;
    h->budget_us = 0;
    history_seal();
    if (s_clock.history_status == RTC_HISTORY_STORE_IO_ERROR) history_open();
    if (s_clock.history_status == RTC_HISTORY_STORE_IO_ERROR || s_clock.history_status == RTC_HISTORY_STORE_BLOCKED) return;
    if (!h->count) return;
    rtc_history_record_t row;
    if (!rtc_history_decode(h->rows[0], 80, &row)) {
        s_clock.history_status = RTC_HISTORY_STORE_BLOCKED;
        return;
    }
    const rtc_history_store_checkpoint_t checkpoint = {h->accepted_sequence, h->dropped};
    s_clock.history_busy = true;
    s_clock.history_status = s_clock.history_save_result = rtc_history_store_append(&s_clock.history->store, &row, &checkpoint);
    if (s_clock.history_status == RTC_HISTORY_STORE_VALID) {
        h->durable_sequence = row.sequence;
        h->generation = s_clock.history->ring.generation;
        history_ack();
        history_refresh();
    }
    s_clock.history_busy = false;
}

rtc_clock_history_summary_t rtc_clock_history_owner_summary(void)
{
    const rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    const rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    const bool ready = s_clock.history && !s_clock.history_busy &&
        (s_clock.history_status == RTC_HISTORY_STORE_VALID || s_clock.history_status == RTC_HISTORY_STORE_MISSING);
    return (rtc_clock_history_summary_t){
        .generation = h->generation, .accepted_sequence = h->accepted_sequence,
        .durable_sequence = h->durable_sequence, .dropped = h->dropped,
        .save_budget_us = h->budget_us, .pending_count = h->count, .flags = h->flags,
        .durable_count = ready ? (uint32_t)s_clock.history->ring.count : 0,
        .ready = ready, .store_status = s_clock.history_status, .save_result = s_clock.history_save_result,
        .active_ppm = a->active_ppm, .active_qualified = a->active_qualified != 0,
        .legacy_candidate_ppm = a->learning.ppm, .legacy_candidate_qualified = a->learning.qualified,
        .calibration = a->calibration,
        .coefficient_saved = a->active_qualified && s_clock.coefficient_saved && s_clock.saved_ppm == a->active_ppm,
        .coefficient_save_blocked = s_clock.coefficient_blocked,
        .saved_ppm = s_clock.saved_ppm, .saved_ppm_valid = s_clock.coefficient_saved,
        .coefficient_save_budget_us = a->coefficient_budget_us,
        .shadow = s_clock.history_shadow
    };
}

bool rtc_clock_history_owner_page(uint64_t generation, size_t cursor, rtc_history_page_t *page)
{
    if (!rtc_clock_history_owner_summary().ready) return false;
    return rtc_history_page(&s_clock.history->ring, generation, cursor, page);
}

void rtc_clock_battery_cache_update(bool valid, uint16_t millivolts, int64_t stamp_us)
{
    if (!s_clock.initialized || s_clock.maintenance) return;
    s_clock.battery_valid = valid && millivolts >= 2000 && millivolts <= 6000 && stamp_us >= 0;
    s_clock.battery_mv = s_clock.battery_valid ? millivolts : 0;
    s_clock.battery_stamp_us = stamp_us;
}

static void clear_session(void)
{
    memset(&rtc_clock_retained.session, 0, sizeof rtc_clock_retained.session);
}

static void seal_accounting(void)
{
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    a->checksum = rtc_clock_checksum(a, offsetof(rtc_clock_accounting_t, checksum));
}

static bool accounting_valid(void)
{
    const rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    if (a->checksum != rtc_clock_checksum(a, offsetof(rtc_clock_accounting_t, checksum))) return false;
    const rtc_clock_calibration_t *calibration = &a->calibration;
    if (calibration->quality > RTC_CLOCK_CALIBRATION_RECOVERY || calibration->uncertainty_valid > 1 ||
        a->last_maintenance_reason > RTC_CLOCK_MAINTENANCE_REJECT ||
        a->maintenance_failures > a->maintenance_attempts ||
        (!calibration->uncertainty_valid && calibration->uncertainty_millippm) ||
        (a->active_qualified != (calibration->quality != RTC_CLOCK_CALIBRATION_NONE)) ||
        ((calibration->evidence_sequence == 0) != (calibration->evidence_utc_us == 0)) ||
        (calibration->evidence_utc_us && (calibration->evidence_utc_us < RTC_HISTORY_UTC_MIN_US ||
            calibration->evidence_utc_us > RTC_HISTORY_UTC_MAX_US))) return false;
    if (calibration->quality <= RTC_CLOCK_CALIBRATION_LEGACY) {
        if (calibration->estimator_version || calibration->uncertainty_valid ||
            (calibration->quality == RTC_CLOCK_CALIBRATION_NONE && calibration->evidence_sequence)) return false;
    } else if (calibration->estimator_version != RTC_HISTORY_ESTIMATE_VERSION ||
        !calibration->uncertainty_valid || !calibration->evidence_sequence) return false;
    return a->version == RTC_CLOCK_VERSION && a->source == s_clock.source && a->continuity &&
        rtc_drift_time_valid(&a->raw_total) && rtc_drift_time_valid(&a->corrected_total) &&
        a->awake_total_us <= RTC_DRIFT_MAX_ELAPSED_US && a->utc_phase_q19 < RTC_DRIFT_Q19 &&
        a->coefficient_budget_us <= RTC_CLOCK_SAVE_INTERVAL_US &&
        a->active_qualified <= 1 && a->active_ppm >= -20000 && a->active_ppm <= 20000 &&
        (a->active_qualified || !a->active_ppm) &&
        ((!a->ratio.denominator && !a->ratio.remainder) ||
         (a->ratio.denominator >= 980000 && a->ratio.denominator <= 1020000 &&
          a->ratio.remainder < a->ratio.denominator)) &&
        a->learning.source == a->source && a->learning.continuity == a->continuity &&
        a->learning.ppm >= -20000 && a->learning.ppm <= 20000 &&
        (a->learning.qualified || !a->learning.ppm);
}

static void reset_learning(void)
{
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    if (++a->continuity == 0) a->continuity = 1;
    rtc_drift_learning_reset(&a->learning, a->source, a->continuity,
        a->learning.ppm, a->learning.qualified);
    a->learning_reason = RTC_DRIFT_LEARN_CONTINUITY;
}

static void initialize_runtime(bool deep_wakeup, bool time_trusted)
{
    s_clock.initialized = true;
    s_clock.source = rtc_clock_platform_source();
    s_clock.supported = rtc_clock_platform_supported() && (s_clock.source == 0 || s_clock.source == 2);
    s_clock.restore_authorized = time_trusted;
    const bool retained_valid = rtc_clock_retained.magic == RTC_CLOCK_RETENTION_MAGIC && accounting_valid();
    s_clock.continuity_valid = deep_wakeup && retained_valid;
    s_clock.history_retained_valid = s_clock.continuity_valid && history_retained_valid();
    int32_t ppm = 0;
    const rtc_clock_store_result_t coefficient = rtc_clock_platform_load_coefficient(&ppm);
    s_clock.coefficient_saved = coefficient == RTC_CLOCK_STORE_VALID && ppm >= -20000 && ppm <= 20000;
    s_clock.coefficient_blocked = coefficient == RTC_CLOCK_STORE_BLOCKED ||
        (coefficient == RTC_CLOCK_STORE_VALID && !s_clock.coefficient_saved);
    s_clock.saved_ppm = s_clock.coefficient_saved ? ppm : 0;
    if (!s_clock.continuity_valid) {
        const rtc_clock_accounting_t previous = rtc_clock_retained.accounting;
        memset(&rtc_clock_retained.accounting, 0, sizeof rtc_clock_retained.accounting);
        rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
        a->version = RTC_CLOCK_VERSION;
        a->source = s_clock.source;
        a->continuity = 1;
        a->active_ppm = s_clock.saved_ppm;
        a->active_qualified = s_clock.coefficient_saved;
        a->calibration.quality = s_clock.coefficient_saved ? RTC_CLOCK_CALIBRATION_LEGACY : RTC_CLOCK_CALIBRATION_NONE;
        if (retained_valid) {
            /* Recover only rate/provenance/budget. A reset never restores UTC,
             * learning baselines, fractional phase or elapsed-time continuity. */
            if (previous.active_qualified) {
                a->active_ppm = previous.active_ppm;
                a->active_qualified = 1;
                a->calibration = previous.calibration;
            }
            a->coefficient_budget_us = previous.coefficient_budget_us;
            a->maintenance_attempts = previous.maintenance_attempts;
            a->maintenance_failures = previous.maintenance_failures;
            a->last_maintenance_reason = previous.last_maintenance_reason;
        }
        rtc_drift_learning_reset(&a->learning, a->source, a->continuity,
            a->active_ppm, a->active_qualified);
        seal_accounting();
    }
    s_clock.execution_hrt_us = s_clock.budget_hrt_us = rtc_clock_platform_uptime_us();
    s_clock.execution_valid = s_clock.execution_hrt_us >= 0;
    s_clock.execution_start_us = s_clock.execution_hrt_us;
    if (!s_clock.continuity_valid) {
        rtc_drift_training_clear(&rtc_clock_retained.training);
        memset(&rtc_clock_retained.model, 0, sizeof rtc_clock_retained.model);
        rtc_clock_retained.next_save_rtc_us = 0;
        rtc_clock_retained.maintenance_blocked = 0;
        rtc_clock_retained.magic = RTC_CLOCK_RETENTION_MAGIC;
    }
    const rtc_clock_store_result_t loaded = rtc_clock_platform_load(&s_clock.saved_model);
    s_clock.saved_valid = loaded == RTC_CLOCK_STORE_VALID && rtc_drift_model_validate(&s_clock.saved_model);
    s_clock.save_blocked = loaded == RTC_CLOCK_STORE_BLOCKED ||
                         (loaded == RTC_CLOCK_STORE_VALID && !s_clock.saved_valid);
    if (s_clock.saved_valid) {
        if (!deep_wakeup || !rtc_drift_model_validate(&rtc_clock_retained.model)) {
            rtc_clock_retained.model = s_clock.saved_model;
        }
        if (!deep_wakeup) {
            rtc_clock_retained.next_save_rtc_us = rtc_clock_platform_rtc_us() + RTC_CLOCK_SAVE_INTERVAL_US;
        }
    }
    if (!deep_wakeup) clear_session();
}

/* 这是相位配对窗口，不是对 RTC 频率或长期精度的承诺。 */
#define CLOCK_CAPTURE_MAX_US INT64_C(1000)

static bool capture_ticks(uint64_t *tick, int64_t *hrt)
{
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const int64_t before = rtc_clock_platform_uptime_us();
        const uint64_t value = rtc_clock_platform_ticks();
        const int64_t after = rtc_clock_platform_uptime_us();
        if (before < 0 || after < before || after - before > CLOCK_CAPTURE_MAX_US) continue;
        *tick = value;
        *hrt = before + (after - before) / 2;
        return true;
    }
    return false;
}

static bool capture_wall_tick(int64_t *utc_anchor, uint64_t *tick, int64_t *hrt)
{
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const int64_t wall_before = rtc_clock_platform_uptime_us();
        int64_t wall;
        const bool got_time = rtc_clock_platform_get_wall_us(&wall);
        const int64_t wall_after = rtc_clock_platform_uptime_us();
        if (!got_time || wall < 0 || wall_before < 0 || wall_after < wall_before ||
            wall_after - wall_before > CLOCK_CAPTURE_MAX_US) continue;
        const uint64_t value = rtc_clock_platform_ticks();
        const int64_t rtc_after = rtc_clock_platform_uptime_us();
        if (rtc_after < wall_after || rtc_after - wall_after > CLOCK_CAPTURE_MAX_US) continue;
        const int64_t wall_mid = wall_before + (wall_after - wall_before) / 2;
        const int64_t rtc_mid = wall_after + (rtc_after - wall_after) / 2;
        const int64_t adjustment = rtc_mid - wall_mid;
        if (adjustment > INT64_MAX - wall) continue;
        *utc_anchor = wall + adjustment;
        *tick = value;
        *hrt = rtc_mid;
        return true;
    }
    return false;
}

/* SDK 对照只用于诊断，不参与恢复目标；失败时仍可用独立账本恢复。 */
static bool sample_sdk_correction(int64_t target_at_tick, int64_t hrt_anchor, int64_t *correction)
{
    const int64_t before = rtc_clock_platform_uptime_us();
    int64_t sdk_utc;
    const bool got_time = rtc_clock_platform_get_wall_us(&sdk_utc);
    const int64_t after = rtc_clock_platform_uptime_us();
    if (!got_time || sdk_utc < 0 || before < hrt_anchor || after < before ||
        after - before > CLOCK_CAPTURE_MAX_US) return false;
    const int64_t delta = before + (after - before) / 2 - hrt_anchor;
    if (delta > INT64_MAX - target_at_tick) return false;
    *correction = target_at_tick + delta - sdk_utc;
    return true;
}

rtc_clock_boot_result_t rtc_clock_boot(bool deep_wakeup, bool time_trusted)
{
    if (!s_clock.initialized) initialize_runtime(deep_wakeup, time_trusted);
    if (s_clock.boot_done) return s_clock.boot_result;
    rtc_clock_session_t *session = &rtc_clock_retained.session;
    if (!deep_wakeup || !session->magic) {
        if (deep_wakeup) {
            reset_learning();
            seal_accounting();
        }
        s_clock.boot_done = true;
        return s_clock.boot_result = deep_wakeup && time_trusted ? RTC_CLOCK_NEEDS_SYNC : RTC_CLOCK_BASELINE;
    }
    if (!s_clock.supported || !s_clock.continuity_valid || !rtc_clock_session_valid(session) ||
        session->source != s_clock.source || session->continuity != rtc_clock_retained.accounting.continuity ||
        session->ledger.phase != RTC_DRIFT_LEDGER_ACTIVE || session->fault) {
        reset_learning();
        seal_accounting();
        s_clock.boot_done = true;
        return s_clock.boot_result = RTC_CLOCK_NEEDS_SYNC;
    }
    if (!session->utc_trusted || !s_clock.restore_authorized) {
        rtc_drift_ledger_consume(&session->ledger);
        session->utc_trusted = 0;
        reset_learning();
        seal_accounting();
        rtc_clock_session_seal(session);
        s_clock.boot_done = true;
        return s_clock.boot_result = time_trusted ? RTC_CLOCK_NEEDS_SYNC : RTC_CLOCK_BASELINE;
    }
    int64_t hrt_anchor;
    uint64_t tick;
    if (!capture_ticks(&tick, &hrt_anchor)) return RTC_CLOCK_NEEDS_SYNC;
    rtc_drift_time_t raw, corrected;
    rtc_clock_accounting_t next = rtc_clock_retained.accounting;
    if (!rtc_drift_ledger_value_at(&session->ledger, tick, &raw.us, &raw.fraction_q19) ||
        !rtc_drift_scale_time(&raw, session->ppm, &next.ratio, &corrected) ||
        !rtc_drift_time_add(&next.raw_total, &raw, &next.raw_total) ||
        !rtc_drift_time_add(&next.corrected_total, &corrected, &next.corrected_total)) return RTC_CLOCK_NEEDS_SYNC;
    if (next.learning.baseline_valid) {
        if (next.learning.baseline.uncertainty_us > UINT64_MAX - 2000) return RTC_CLOCK_NEEDS_SYNC;
        next.learning.baseline.uncertainty_us += 2000;
    }
    const uint32_t phase = session->anchor_phase_q19 + corrected.fraction_q19;
    const uint64_t elapsed = corrected.us + phase / RTC_DRIFT_Q19;
    next.utc_phase_q19 = phase % RTC_DRIFT_Q19;
    if (session->utc_anchor_us < 0 || elapsed > (uint64_t)(INT64_MAX - session->utc_anchor_us))
        return RTC_CLOCK_NEEDS_SYNC;
    int64_t target = session->utc_anchor_us + (int64_t)elapsed;
    int64_t sdk_correction = 0;
    const bool sdk_sample_valid = sample_sdk_correction(target, hrt_anchor, &sdk_correction);
    const int64_t hrt_now = rtc_clock_platform_uptime_us();
    if (hrt_now < hrt_anchor || hrt_now - hrt_anchor > INT64_MAX - target) return RTC_CLOCK_NEEDS_SYNC;
    target += hrt_now - hrt_anchor;
    if (rtc_clock_platform_source() != s_clock.source) {
        reset_learning();
        seal_accounting();
        session->utc_trusted = 0;
        session->fault = RTC_CLOCK_FAULT_SOURCE;
        rtc_clock_session_seal(session);
        s_clock.boot_done = true;
        return s_clock.boot_result = RTC_CLOCK_NEEDS_SYNC;
    }
    /* 替换 SDK 已计算的睡眠贡献，不把 elapsed 再加到当前 UTC。 */
    if (!rtc_clock_platform_set_wall_us(target)) return RTC_CLOCK_NEEDS_SYNC;
    const int64_t applied_at = rtc_clock_platform_uptime_us();
    const bool checked = applied_at >= hrt_now && applied_at - hrt_now <= CLOCK_CAPTURE_MAX_US;
    const uint64_t budget_left = RTC_CLOCK_SAVE_INTERVAL_US - next.coefficient_budget_us;
    next.coefficient_budget_us += corrected.us < budget_left ? corrected.us : budget_left;
    rtc_clock_retained.accounting = next;
    history_budget(corrected.us);
    s_clock.execution_hrt_us = s_clock.budget_hrt_us = hrt_anchor;
    s_clock.execution_start_us = hrt_anchor;
    s_clock.execution_valid = true;
    rtc_drift_ledger_consume(&session->ledger);
    if (!checked) {
        session->utc_trusted = 0;
        session->fault = RTC_CLOCK_FAULT_CAPTURE;
        reset_learning();
    }
    seal_accounting();
    rtc_clock_session_seal(session);
    s_clock.reconciled_elapsed_us = raw.us;
    s_clock.corrected_elapsed_us = elapsed;
    s_clock.sdk_correction_us = sdk_sample_valid ? sdk_correction : 0;
    s_clock.sdk_sample_valid = sdk_sample_valid;
    s_clock.boot_done = true;
    return s_clock.boot_result = checked ? RTC_CLOCK_RESTORED : RTC_CLOCK_NEEDS_SYNC;
}

static bool model_changed_enough(const rtc_drift_model_t *current)
{
    if (!s_clock.saved_valid) return true;
    const rtc_drift_model_t *saved = &s_clock.saved_model;
    const int32_t lo = current->min_temp_mc > saved->min_temp_mc ? current->min_temp_mc : saved->min_temp_mc;
    const int32_t hi = current->max_temp_mc < saved->max_temp_mc ? current->max_temp_mc : saved->max_temp_mc;
    if (lo > hi) return false;
    double maximum_ppm = 0;
    const int32_t points[3] = {lo, lo + (hi - lo) / 2, hi};
    for (unsigned i = 0; i < 3; ++i) {
        double a, b;
        if (!rtc_drift_model_evaluate(current, points[i], &a) ||
            !rtc_drift_model_evaluate(saved, points[i], &b)) return false;
        const double ppm = fabs(a - b) * 1000000.0 / b;
        if (ppm > maximum_ppm) maximum_ppm = ppm;
    }
    const bool improved = saved->rms_ppm > 0 && current->rms_ppm <= saved->rms_ppm * 0.8;
    const bool expanded = current->min_temp_mc <= saved->min_temp_mc - 2000 ||
                          current->max_temp_mc >= saved->max_temp_mc + 2000;
    return maximum_ppm >= 10.0 && (improved || expanded);
}

static void checkpoint_model(void)
{
    rtc_drift_model_t *model = &rtc_clock_retained.model;
    const uint64_t now = rtc_clock_platform_rtc_us();
    if (s_clock.save_blocked || !rtc_drift_model_validate(model) ||
        now < rtc_clock_retained.next_save_rtc_us || !model_changed_enough(model)) return;
    /* 失败也消耗本次尝试预算，不在下一次 tick 紧密重写。 */
    rtc_clock_retained.next_save_rtc_us = now + RTC_CLOCK_SAVE_INTERVAL_US;
    if (rtc_clock_platform_save(model)) {
        s_clock.saved_model = *model;
        s_clock.saved_valid = true;
    }
}

static void read_temperature(int64_t now)
{
    int32_t value;
    s_clock.temperature.valid = rtc_clock_platform_temperature(&value) && value >= -10000 && value <= 80000;
    s_clock.temperature.stamp_us = now;
    if (s_clock.temperature.valid) s_clock.temperature.temp_mc = value;
}

static bool measure_period(void)
{
    if (rtc_clock_platform_source() != s_clock.source) return false;
    const uint32_t period = rtc_clock_platform_measure();
    if (rtc_clock_platform_source() != s_clock.source || !rtc_drift_period_valid(period)) return false;
    s_clock.direct = (rtc_drift_direct_t){
        .stamp_us = rtc_clock_platform_uptime_us(), .period_q19 = period, .valid = true,
        .temp_mc = s_clock.temperature.valid ? s_clock.temperature.temp_mc : INT32_MIN
    };
    if (s_clock.light_enabled) rtc_light_sleep_platform_set_period(period);
    if (s_clock.temperature.valid) {
        rtc_drift_training_add(&rtc_clock_retained.training, s_clock.temperature.temp_mc, period);
        rtc_drift_model_fit(&rtc_clock_retained.training, &rtc_clock_retained.model);
    }
    return true;
}

static void advance_budget(int64_t hrt)
{
    if (hrt < s_clock.budget_hrt_us || s_clock.budget_hrt_us < 0) return;
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    const uint64_t delta = (uint64_t)(hrt - s_clock.budget_hrt_us);
    const uint64_t left = RTC_CLOCK_SAVE_INTERVAL_US - a->coefficient_budget_us;
    a->coefficient_budget_us += delta < left ? delta : left;
    history_budget(delta);
    s_clock.budget_hrt_us = hrt;
}

bool rtc_clock_start_light_sleep(void)
{
    if (!s_clock.initialized || !s_clock.supported || s_clock.provisional || s_clock.light_fault)
        return false;
    if (s_clock.light_enabled) return true;
    if (!measure_period() || !rtc_light_sleep_platform_start(s_clock.direct.period_q19, s_clock.source))
        return false;
    s_clock.light_seen = rtc_light_sleep_platform_snapshot();
    s_clock.light_enabled = true;
    return true;
}

static bool light_sleep_fault(int64_t hrt)
{
    rtc_light_sleep_platform_inhibit();
    s_clock.light_fault = true;
    s_clock.light_needs_sync = true;
    reset_learning();
    /* 未知区间不进入独立清醒坐标；重新校时后可从新坐标继续学习。 */
    s_clock.execution_hrt_us = s_clock.execution_start_us = hrt;
    s_clock.execution_valid = hrt >= 0;
    s_clock.boot_result = RTC_CLOCK_NEEDS_SYNC;
    seal_accounting();
    return false;
}

static bool service_light_sleep(bool correct_utc)
{
    if (!s_clock.light_enabled) return true;
    if (s_clock.light_fault) return false;
    const rtc_light_sleep_snapshot_t sample = rtc_light_sleep_platform_snapshot();
    const rtc_light_sleep_snapshot_t *old = &s_clock.light_seen;
    if (sample.fault || sample.hrt_us < s_clock.execution_hrt_us ||
        sample.sdk_total_us < old->sdk_total_us || sample.sleep_count < old->sleep_count ||
        sample.uncertainty_total_us < old->uncertainty_total_us ||
        rtc_clock_platform_source() != s_clock.source)
        return light_sleep_fault(sample.hrt_us);
    if (sample.sleep_count == old->sleep_count) return true;
    int64_t offset_delta;
    if (__builtin_sub_overflow(sample.wall_offset_total_us, old->wall_offset_total_us, &offset_delta))
        return light_sleep_fault(sample.hrt_us);
    const uint64_t sdk = sample.sdk_total_us - old->sdk_total_us;
    const uint64_t elapsed = (uint64_t)(sample.hrt_us - s_clock.execution_hrt_us);
    const uint64_t uncertainty = sample.uncertainty_total_us - old->uncertainty_total_us;
    rtc_clock_accounting_t next = rtc_clock_retained.accounting;
    rtc_drift_time_t raw, corrected;
    if (!s_clock.execution_valid || sample.last_exit_hrt_us > sample.hrt_us ||
        sample.last_exit_hrt_us < old->last_exit_hrt_us || sdk > elapsed ||
        elapsed - sdk > RTC_DRIFT_MAX_ELAPSED_US - next.awake_total_us ||
        !rtc_drift_time_sub(&sample.raw_total, &old->raw_total, &raw) ||
        !rtc_drift_scale_time(&raw, next.active_qualified ? next.active_ppm : 0, &next.ratio, &corrected) ||
        !rtc_drift_time_add(&next.raw_total, &raw, &next.raw_total) ||
        !rtc_drift_time_add(&next.corrected_total, &corrected, &next.corrected_total) ||
        (next.learning.baseline_valid && uncertainty > UINT64_MAX - next.learning.baseline.uncertainty_us))
        return light_sleep_fault(sample.hrt_us);
    next.awake_total_us += elapsed - sdk;
    if (next.learning.baseline_valid) next.learning.baseline.uncertainty_us += uncertainty;
    const uint32_t phase = next.utc_phase_q19 + corrected.fraction_q19;
    int64_t correction = (int64_t)(corrected.us + phase / RTC_DRIFT_Q19) - (int64_t)sdk;
    /* SDK 还会改变 libc 的 UTC 偏移；连同 HRT 替换量一起扣除。 */
    if (__builtin_sub_overflow(correction, offset_delta, &correction))
        return light_sleep_fault(sample.hrt_us);
    next.utc_phase_q19 = phase % RTC_DRIFT_Q19;
    if (correct_utc && correction) {
        const int64_t before = rtc_clock_platform_uptime_us();
        int64_t wall;
        if (!rtc_clock_platform_get_wall_us(&wall)) return light_sleep_fault(sample.hrt_us);
        const int64_t after = rtc_clock_platform_uptime_us();
        int64_t target;
        if (before < sample.hrt_us || after < before || after - before > CLOCK_CAPTURE_MAX_US ||
            __builtin_add_overflow(wall, correction, &target) || target < 0)
            return light_sleep_fault(after);
        const int64_t adjustment = (after - before + 1) / 2;
        if (target > INT64_MAX - adjustment || !rtc_clock_platform_set_wall_us(target + adjustment))
            return light_sleep_fault(after);
        const int64_t applied = rtc_clock_platform_uptime_us();
        if (applied < after || applied - after > CLOCK_CAPTURE_MAX_US) return light_sleep_fault(applied);
    }
    /* 不修改 SDK 单调时钟；只结算 UTC 差额与应用自有学习坐标。 */
    advance_budget(sample.hrt_us);
    next.coefficient_budget_us = rtc_clock_retained.accounting.coefficient_budget_us;
    rtc_clock_retained.accounting = next;
    s_clock.execution_hrt_us = sample.hrt_us;
    s_clock.light_seen = sample;
    seal_accounting();
    return true;
}

bool rtc_clock_service_light_sleep(void)
{
    return service_light_sleep(true);
}

static void checkpoint_coefficient(void)
{
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    advance_budget(rtc_clock_platform_uptime_us());
    const int32_t ppm = a->active_ppm;
    const int32_t change = ppm - s_clock.saved_ppm;
    if (!s_clock.coefficient_blocked && a->active_qualified &&
        a->coefficient_budget_us >= RTC_CLOCK_SAVE_INTERVAL_US &&
        (!s_clock.coefficient_saved || change != 0)) {
        a->coefficient_budget_us = 0;
        if (rtc_clock_platform_save_coefficient(ppm)) {
            s_clock.saved_ppm = ppm;
            s_clock.coefficient_saved = true;
        }
    }
    seal_accounting();
}

void rtc_clock_tick(void)
{
    if (!s_clock.initialized || !s_clock.supported || s_clock.maintenance || s_clock.provisional) return;
    (void)rtc_clock_service_light_sleep();
    if (rtc_clock_platform_source() != s_clock.source) {
        reset_learning();
        seal_accounting();
        return;
    }
    history_start();
    advance_budget(rtc_clock_platform_uptime_us());
    history_checkpoint();
    if (s_clock.history && !s_clock.history_busy &&
        s_clock.history_status == RTC_HISTORY_STORE_IO_ERROR &&
        rtc_clock_platform_uptime_us() >= s_clock.history_retry_us) history_open();
    const int64_t now = rtc_clock_platform_uptime_us();
    if (now >= s_clock.next_sample_us) {
        s_clock.next_sample_us = now + 60000000;
        history_refresh();
        read_temperature(now);
        measure_period();
        checkpoint_model();
    }
    checkpoint_coefficient();
}

bool rtc_clock_prepare_sleep(void)
{
    if (!s_clock.initialized || s_clock.provisional) return false;
    (void)rtc_clock_service_light_sleep();
    if (rtc_clock_platform_source() != s_clock.source) {
        s_clock.prepared_period = 0;
        reset_learning();
        seal_accounting();
        return false;
    }
    rtc_clock_begin_normal_wake();
    s_clock.prepared_period = 0;
    if (s_clock.supported) {
        const int64_t now = rtc_clock_platform_uptime_us();
        read_temperature(now);
        /* 拟合残差不是实机精度证明；睡前 UTC 时基只采用本次长校准。 */
        if (measure_period()) s_clock.prepared_period = s_clock.direct.period_q19;
        checkpoint_model();
    }
    if (!rtc_clock_platform_sensor_stop()) {
        s_clock.prepared_period = 0;
        return false;
    }
    return true;
}

bool rtc_clock_commit_sleep(bool time_trusted)
{
    if (!s_clock.initialized || s_clock.provisional) return false;
    (void)rtc_clock_service_light_sleep();
    time_trusted = time_trusted && !s_clock.light_needs_sync;
    s_clock.backup_session = rtc_clock_retained.session;
    s_clock.backup_maintenance_count = rtc_clock_retained.accounting.maintenance_count;
    if (!rtc_drift_period_valid(s_clock.prepared_period)) {
        clear_session();
        rtc_clock_platform_arm(false);
        s_clock.provisional = true;
        return true;
    }
    if (!s_clock.supported || rtc_clock_platform_source() != s_clock.source) return false;
    rtc_clock_session_t next = {0};
    uint64_t tick;
    int64_t utc = 0, hrt = 0;
    const bool captured = time_trusted && capture_wall_tick(&utc, &tick, &hrt);
    if (!captured && !capture_ticks(&tick, &hrt)) return false;
    if (rtc_clock_platform_source() != s_clock.source) return false;
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    if (!s_clock.execution_valid || hrt < s_clock.execution_hrt_us ||
        (uint64_t)(hrt - s_clock.execution_hrt_us) > RTC_DRIFT_MAX_ELAPSED_US - a->awake_total_us) return false;
    if (!rtc_drift_ledger_start(&next.ledger, tick, s_clock.prepared_period)) return false;
    a->awake_total_us += (uint64_t)(hrt - s_clock.execution_hrt_us);
    s_clock.execution_hrt_us = hrt;
    advance_budget(hrt);
    next.source = s_clock.source;
    next.ppm = a->active_qualified ? a->active_ppm : 0;
    next.anchor_phase_q19 = a->utc_phase_q19;
    next.continuity = a->continuity;
    next.utc_trusted = captured;
    if (captured) next.utc_anchor_us = utc;
    else if (time_trusted) next.fault = RTC_CLOCK_FAULT_CAPTURE;
    next.magic = RTC_CLOCK_MAGIC;
    next.version = RTC_CLOCK_VERSION;
    const bool arm_maintenance = captured && s_clock.supported && !rtc_clock_retained.maintenance_blocked;
    next.maintenance_enabled = arm_maintenance ? rtc_clock_platform_arm_interval(
        s_clock.maintenance ? RTC_CLOCK_STEADY_WAKE_US : RTC_CLOCK_COOLDOWN_WAKE_US) :
        rtc_clock_platform_arm(false);
    if (s_clock.maintenance && a->maintenance_count != UINT32_MAX) ++a->maintenance_count;
    rtc_clock_session_seal(&next);
    rtc_clock_retained.session = next;
    s_clock.provisional = true;
    seal_accounting();
    return true;
}

bool rtc_clock_maintenance_due(bool timer_only)
{
    const rtc_clock_session_t *session = &rtc_clock_retained.session;
    return timer_only && s_clock.initialized && s_clock.supported && !s_clock.provisional &&
        s_clock.boot_done && s_clock.boot_result == RTC_CLOCK_RESTORED &&
        rtc_clock_session_valid(session) && accounting_valid() &&
        session->source == rtc_clock_platform_source() &&
        session->continuity == rtc_clock_retained.accounting.continuity &&
        session->utc_trusted && !session->fault && session->maintenance_enabled &&
        !rtc_clock_retained.maintenance_blocked;
}

bool rtc_clock_prepare_maintenance(void)
{
    if (!rtc_clock_maintenance_due(true)) return false;
    s_clock.prepared_period = 0;
    s_clock.maintenance = true;
    /* Use the existing 24h budget even when there is no normal user wake.
     * Complete Flash work before measuring the period used for the next sleep. */
    checkpoint_coefficient();
    const uint32_t period = rtc_clock_platform_measure();
    const bool source_changed = rtc_clock_platform_source() != s_clock.source;
    if (!rtc_drift_period_valid(period) || source_changed) {
        rtc_clock_retained.maintenance_blocked = 1;
        rtc_clock_retained.session.fault = source_changed ? RTC_CLOCK_FAULT_SOURCE : RTC_CLOCK_FAULT_CALIBRATION;
        if (source_changed) reset_learning();
        seal_accounting();
        rtc_clock_session_seal(&rtc_clock_retained.session);
        return false;
    }
    s_clock.prepared_period = period;
    return true;
}

void rtc_clock_note_maintenance(rtc_clock_maintenance_reason_t reason)
{
    if (!s_clock.initialized || (unsigned)reason > RTC_CLOCK_MAINTENANCE_REJECT) return;
    rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    if (reason == RTC_CLOCK_MAINTENANCE_ATTEMPT && a->maintenance_attempts != UINT32_MAX)
        ++a->maintenance_attempts;
    if (reason != RTC_CLOCK_MAINTENANCE_NONE && reason != RTC_CLOCK_MAINTENANCE_ATTEMPT &&
        reason != RTC_CLOCK_MAINTENANCE_KEY && a->maintenance_failures < a->maintenance_attempts)
        ++a->maintenance_failures;
    a->last_maintenance_reason = reason;
    seal_accounting();
}

void rtc_clock_begin_normal_wake(void)
{
    s_clock.maintenance = false;
    history_start();
    seal_accounting();
}

bool rtc_clock_abort_sleep(void)
{
    if (!s_clock.initialized) return false;
    rtc_clock_platform_arm(false);
    if (s_clock.provisional) {
        rtc_clock_retained.session = s_clock.backup_session;
        rtc_clock_retained.accounting.maintenance_count = s_clock.backup_maintenance_count;
        s_clock.provisional = false;
    }
    rtc_clock_retained.session.maintenance_enabled = 0;
    rtc_clock_session_seal(&rtc_clock_retained.session);
    s_clock.prepared_period = 0;
    s_clock.maintenance = false;
    seal_accounting();
    return true;
}

rtc_clock_diagnostics_t rtc_clock_diagnostics(void)
{
    const rtc_clock_session_t *session = &rtc_clock_retained.session;
    const bool qualified = rtc_drift_model_validate(&rtc_clock_retained.model);
    const rtc_clock_accounting_t *a = &rtc_clock_retained.accounting;
    uint64_t awake = a->awake_total_us;
    const int64_t now = rtc_clock_platform_uptime_us();
    if (s_clock.execution_valid && now >= s_clock.execution_hrt_us &&
        (uint64_t)(now - s_clock.execution_hrt_us) <= RTC_DRIFT_MAX_ELAPSED_US - awake)
        awake += (uint64_t)(now - s_clock.execution_hrt_us);
    return (rtc_clock_diagnostics_t){
        .checkpoint_elapsed_us = session->ledger.elapsed_us,
        .reconciled_elapsed_us = s_clock.reconciled_elapsed_us,
        .sdk_correction_us = s_clock.sdk_correction_us,
        .sdk_sample_valid = s_clock.sdk_sample_valid,
        .period_q19 = session->ledger.period_q19,
        .samples = session->samples,
        .pre_tail_gap_upper_us = session->pre_tail_gap_upper_us,
        .fault = session->fault,
        .model_observations = qualified ? rtc_clock_retained.model.observations : rtc_clock_retained.training.count,
        .model_rms_millippm = qualified ? (uint32_t)(rtc_clock_retained.model.rms_ppm * 1000.0 + 0.5) : 0,
        .utc_restored = s_clock.boot_done && s_clock.boot_result == RTC_CLOCK_RESTORED,
        .user_wake = session->user_wake != 0,
        .maintenance_blocked = rtc_clock_retained.maintenance_blocked != 0,
        .model_qualified = qualified,
        .source = a->source,
        .ppm = a->active_ppm,
        .ppm_qualified = a->active_qualified != 0,
        .corrected_elapsed_us = s_clock.corrected_elapsed_us,
        .raw_chain_us = a->raw_total.us,
        .corrected_chain_us = a->corrected_total.us,
        .awake_total_us = awake,
        .maintenance_count = a->maintenance_count,
        .maintenance_attempts = a->maintenance_attempts,
        .maintenance_failures = a->maintenance_failures,
        .last_maintenance_reason = a->last_maintenance_reason,
        .learning_reason = a->learning_reason,
        .reference_status = (rtc_clock_reference_result_t)a->reference_status
    };
}

void rtc_clock_wall_time_set(void)
{
    if (!s_clock.initialized) initialize_runtime(false, true);
    /* 外部墙钟已经成功设置；仅结算坐标，不能把旧 UTC 差额叠加到新参考。 */
    (void)service_light_sleep(false);
    s_clock.light_needs_sync = false;
    if (s_clock.provisional) rtc_clock_abort_sleep();
    clear_session();
    reset_learning();
    rtc_clock_retained.accounting.utc_phase_q19 = 0;
    rtc_clock_retained.accounting.ratio = (rtc_drift_ratio_t){0};
    rtc_clock_retained.accounting.maintenance_count = 0;
    seal_accounting();
    s_clock.reconciled_elapsed_us = 0;
    s_clock.corrected_elapsed_us = 0;
    s_clock.sdk_correction_us = 0;
    s_clock.sdk_sample_valid = false;
    s_clock.boot_done = true;
    s_clock.boot_result = RTC_CLOCK_BASELINE;
}

static bool awake_at(int64_t hrt, uint64_t *awake)
{
    const uint64_t total = rtc_clock_retained.accounting.awake_total_us;
    if (!s_clock.execution_valid || hrt < s_clock.execution_start_us ||
        (s_clock.light_enabled && hrt < s_clock.light_seen.last_exit_hrt_us)) return false;
    if (hrt < s_clock.execution_hrt_us) {
        const uint64_t age = (uint64_t)(s_clock.execution_hrt_us - hrt);
        if (age > total) return false;
        *awake = total - age;
    } else {
        const uint64_t delta = (uint64_t)(hrt - s_clock.execution_hrt_us);
        if (delta > RTC_DRIFT_MAX_ELAPSED_US - total) return false;
        *awake = total + delta;
    }
    return true;
}

static bool history_window(rtc_history_record_t *row, const rtc_drift_reference_t *old,
    const rtc_drift_reference_t *sample)
{
    rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
    if (!s_clock.history_started || !s_clock.history_retained_valid || h->accepted_sequence == UINT64_MAX ||
        sample->utc_us <= old->utc_us || sample->awake_us < old->awake_us) return false;
    *row = (rtc_history_record_t){
        .source = sample->source, .sequence = h->accepted_sequence + 1, .epoch = h->epoch,
        .utc_end_us = sample->utc_us, .awake_delta_us = sample->awake_us - old->awake_us,
        .utc_delta_us = sample->utc_us - old->utc_us
    };
    if (!rtc_drift_time_sub(&sample->raw, &old->raw, &row->raw)) return false;
    const uint64_t awake_uncertainty = row->awake_delta_us / 20000 + (row->awake_delta_us % 20000 != 0);
    if (old->uncertainty_us > UINT64_MAX - sample->uncertainty_us ||
        old->uncertainty_us + sample->uncertainty_us > UINT64_MAX - awake_uncertainty) return false;
    row->combined_uncertainty_us = old->uncertainty_us + sample->uncertainty_us + awake_uncertainty;
    return rtc_history_record_valid(row);
}

static uint16_t history_age(int64_t now, int64_t stamp)
{
    const uint64_t seconds = (uint64_t)(now - stamp) / 1000000;
    return seconds > UINT16_MAX ? UINT16_MAX : (uint16_t)seconds;
}

static void history_environment(rtc_history_record_t *row, int64_t hrt)
{
    if (s_clock.temperature.valid && s_clock.temperature.stamp_us >= 0 &&
        s_clock.temperature.stamp_us <= hrt && hrt - s_clock.temperature.stamp_us <= INT64_C(120000000)) {
        row->flags |= RTC_HISTORY_TEMP_VALID | RTC_HISTORY_AWAKE_ENDPOINT_ONLY;
        row->temperature_centi_c = (int16_t)(s_clock.temperature.temp_mc / 10);
        row->temp_age_s = history_age(hrt, s_clock.temperature.stamp_us);
    }
    if (s_clock.battery_valid && s_clock.battery_stamp_us <= hrt &&
        hrt - s_clock.battery_stamp_us <= INT64_C(120000000)) {
        row->flags |= RTC_HISTORY_BATTERY_VALID | RTC_HISTORY_AWAKE_ENDPOINT_ONLY;
        row->battery_mv = s_clock.battery_mv;
        row->battery_age_s = history_age(hrt, s_clock.battery_stamp_us);
    }
}

rtc_clock_reference_result_t rtc_clock_apply_reference(const rtc_clock_reference_t *reference)
{
    if (!reference || !s_clock.initialized || s_clock.provisional) return RTC_CLOCK_REFERENCE_FAILED;
    (void)rtc_clock_service_light_sleep();
    const int64_t now = rtc_clock_platform_uptime_us();
    const uint64_t minimum = UINT64_C(1577836800) * 1000000;
    const uint64_t maximum = UINT64_C(4102444800) * 1000000;
    if (reference->hrt_us < 0 || now < reference->hrt_us || reference->utc_us < minimum ||
        reference->utc_us > maximum) return RTC_CLOCK_REFERENCE_FAILED;
    const uint64_t age = (uint64_t)(now - reference->hrt_us);
    if (age > maximum - reference->utc_us ||
        (reference->max_age_us && age > reference->max_age_us)) return RTC_CLOCK_REFERENCE_FAILED;
    rtc_clock_accounting_t next = rtc_clock_retained.accounting;
    rtc_history_record_t history_row;
    bool capture_history = false;
    bool evidence_dropped = false;
    uint64_t reference_awake = 0;
    const bool eligible = s_clock.boot_done && reference->quality && age <= UINT64_C(5000000) && s_clock.supported &&
        awake_at(reference->hrt_us, &reference_awake) && rtc_clock_platform_source() == s_clock.source;
    if (eligible) {
        const rtc_drift_reference_t sample = {
            .source = next.source, .continuity = next.continuity,
            .reference_id = reference->id, .utc_us = reference->utc_us,
            .awake_us = reference_awake,
            .raw = next.raw_total, .uncertainty_us = reference->uncertainty_us
        };
        next.learning_reason = rtc_drift_learn(&next.learning, &sample);
        if (next.learning_reason == RTC_DRIFT_LEARN_PENDING || next.learning_reason == RTC_DRIFT_LEARN_PUBLISHED ||
            next.learning_reason == RTC_DRIFT_LEARN_INCONSISTENT) {
            capture_history = history_window(&history_row, &rtc_clock_retained.accounting.learning.baseline, &sample);
            if (capture_history && rtc_clock_retained.history.count == 2) {
                evidence_dropped = true;
                const rtc_drift_learning_t advanced = next.learning;
                next.learning = rtc_clock_retained.accounting.learning;
                next.learning.baseline = advanced.baseline;
                next.learning.baseline_valid = advanced.baseline_valid;
                next.learning.last_reference_id = advanced.last_reference_id;
                next.learning.last_seen_utc_us = advanced.last_seen_utc_us;
            }
        }
    }
    const int64_t before = rtc_clock_platform_uptime_us();
    if (before < now || (uint64_t)(before - reference->hrt_us) > maximum - reference->utc_us ||
        (reference->max_age_us && (uint64_t)(before - reference->hrt_us) > reference->max_age_us))
        return RTC_CLOCK_REFERENCE_FAILED;
    const int64_t target = (int64_t)(reference->utc_us + (uint64_t)(before - reference->hrt_us));
    if (!rtc_clock_platform_set_wall_us(target)) return RTC_CLOCK_REFERENCE_FAILED;
    const int64_t after = rtc_clock_platform_uptime_us();
    const bool checked = after >= before && after - before <= CLOCK_CAPTURE_MAX_US;
    if (checked) {
        const bool unavailable = s_clock.history_status == RTC_HISTORY_STORE_IO_ERROR;
        const bool fallback = unavailable && !evidence_dropped && capture_history &&
            next.learning_reason == RTC_DRIFT_LEARN_PUBLISHED;
        if (eligible && next.learning.qualified && (!next.active_qualified || fallback) &&
            (!fallback || (history_row.sequence > next.calibration.evidence_sequence &&
                history_row.utc_end_us >= next.calibration.evidence_utc_us))) {
            if (!next.active_qualified || next.active_ppm != next.learning.ppm) {
                next.calibration = (rtc_clock_calibration_t){
                    .quality = RTC_CLOCK_CALIBRATION_LEGACY,
                    .evidence_sequence = capture_history ? history_row.sequence : 0,
                    .evidence_utc_us = capture_history ? history_row.utc_end_us : 0
                };
            }
            next.active_ppm = next.learning.ppm;
            next.active_qualified = 1;
        }
        s_clock.light_needs_sync = false;
        rtc_clock_retained.accounting = next;
    }
    if (checked && capture_history) {
        rtc_clock_history_retained_t *h = &rtc_clock_retained.history;
        ++h->accepted_sequence;
        if (h->count == 2) {
            ++h->dropped;
            h->flags |= 2;
        } else {
            history_environment(&history_row, reference->hrt_us);
            if (rtc_history_encode(&history_row, h->rows[h->count], 80)) ++h->count;
            else { ++h->dropped; h->flags |= 2; }
        }
        history_seal();
    }
    if (!checked || !eligible) reset_learning();
    rtc_clock_retained.accounting.utc_phase_q19 = 0;
    rtc_clock_retained.accounting.ratio = (rtc_drift_ratio_t){0};
    rtc_clock_retained.accounting.maintenance_count = 0;
    rtc_clock_retained.accounting.reference_status = checked ?
        RTC_CLOCK_REFERENCE_SET : RTC_CLOCK_REFERENCE_UNCERTAIN;
    seal_accounting();
    clear_session();
    s_clock.boot_done = true;
    s_clock.boot_result = checked ? RTC_CLOCK_BASELINE : RTC_CLOCK_NEEDS_SYNC;
    s_clock.reconciled_elapsed_us = s_clock.corrected_elapsed_us = 0;
    s_clock.sdk_sample_valid = false;
    s_clock.sdk_correction_us = 0;
    return checked ? RTC_CLOCK_REFERENCE_SET : RTC_CLOCK_REFERENCE_UNCERTAIN;
}
