/* 编译真实状态机，仅替换 SDK I/O 边界；不读取设备或真实 NVS。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtc_clock_internal.h"
#include "rtc_light_sleep.h"

rtc_clock_retained_t rtc_clock_retained;
static uint64_t ticks, rtc_us;
static int64_t uptime_us, wall_us, applied_us;
static uint32_t period;
static int32_t temperature;
static bool fail_set, fail_get, fail_sensor, fail_stop, fail_save;
static unsigned sets, saves, stops, reads, measures;
static rtc_clock_store_result_t store_state;
static rtc_drift_model_t stored_model;
static int64_t tick_delay_before_us, tick_delay_after_us, set_delay_us;
static int64_t wall_delay_before_us, wall_delay_after_us;
static unsigned delayed_tick_reads;
static uint32_t source;
static int32_t stored_ppm;
static rtc_clock_store_result_t coefficient_store;
static unsigned coefficient_saves;
static bool fail_coefficient_save;
static bool change_source_on_tick;
static rtc_light_sleep_snapshot_t light_snapshot;
static uint32_t light_period;
static bool light_inhibited, light_start_fail;

bool rtc_light_sleep_platform_start(uint32_t measured_period, uint32_t clock_source) {
    assert(clock_source == source);
    light_period = measured_period;
    return !light_start_fail;
}
void rtc_light_sleep_platform_set_period(uint32_t value) { light_period = value; }
void rtc_light_sleep_platform_inhibit(void) { light_inhibited = true; }
rtc_light_sleep_snapshot_t rtc_light_sleep_platform_snapshot(void) {
    light_snapshot.hrt_us = uptime_us;
    return light_snapshot;
}

uint32_t rtc_clock_platform_source(void) { return source; }
rtc_clock_store_result_t rtc_clock_platform_load_coefficient(int32_t *out) {
    *out = stored_ppm;
    return coefficient_store;
}
bool rtc_clock_platform_save_coefficient(int32_t ppm) {
    ++coefficient_saves;
    if (fail_coefficient_save) return false;
    stored_ppm = ppm;
    coefficient_store = RTC_CLOCK_STORE_VALID;
    return true;
}

static void advance_fake(int64_t us) {
    uptime_us += us;
    wall_us += us;
    rtc_us += (uint64_t)us;
    ticks += (uint64_t)us / 3;
}
uint64_t rtc_clock_platform_ticks(void) {
    const bool delayed = delayed_tick_reads > 0;
    if (delayed) { --delayed_tick_reads; advance_fake(tick_delay_before_us); }
    const uint64_t result = ticks;
    if (change_source_on_tick) source = 2;
    if (delayed) advance_fake(tick_delay_after_us);
    return result;
}
int64_t rtc_clock_platform_uptime_us(void) { return uptime_us; }
uint64_t rtc_clock_platform_rtc_us(void) { return rtc_us; }
bool rtc_clock_platform_get_wall_us(int64_t *out) {
    if (fail_get) return false;
    advance_fake(wall_delay_before_us);
    *out = wall_us;
    advance_fake(wall_delay_after_us);
    return true;
}
bool rtc_clock_platform_set_wall_us(int64_t value) {
    ++sets;
    if (fail_set) return false;
    advance_fake(set_delay_us);
    wall_us = applied_us = value;
    return true;
}
bool rtc_clock_platform_temperature(int32_t *out) {
    ++reads;
    if (fail_sensor) return false;
    *out = temperature;
    return true;
}
bool rtc_clock_platform_sensor_stop(void) { ++stops; return !fail_stop; }
uint32_t rtc_clock_platform_measure(void) { ++measures; return period; }
bool rtc_clock_platform_supported(void) { return true; }
rtc_clock_store_result_t rtc_clock_platform_load(rtc_drift_model_t *out) {
    if (store_state == RTC_CLOCK_STORE_VALID) *out = stored_model;
    return store_state;
}
bool rtc_clock_platform_save(const rtc_drift_model_t *model) {
    ++saves;
    if (fail_save) return false;
    stored_model = *model;
    store_state = RTC_CLOCK_STORE_VALID;
    return true;
}
bool rtc_clock_platform_arm(bool requested) { return requested; }

static uint8_t history_blobs[13][832];
static size_t history_sizes[13];
static unsigned history_reads, history_writes, history_commits, epoch_calls;
static bool history_fail, history_ack_loss, history_init_fail;
static unsigned history_init_calls;
static int history_key(const char *key) {
    const char *p = strchr("0123456789a", key[1]);
    assert(p);
    return (key[0] == 'm' ? 11 : 0) + (int)(p - "0123456789a");
}
static rtc_history_store_result_t history_read(void *u, const char *key, void *out, size_t *size) {
    (void)u;
    ++history_reads;
    int k = history_key(key);
    if (history_fail) return RTC_HISTORY_STORE_IO_ERROR;
    if (history_ack_loss && key[0] == 'm' && history_commits) {
        history_ack_loss = false;
        return RTC_HISTORY_STORE_IO_ERROR;
    }
    if (!history_sizes[k]) return RTC_HISTORY_STORE_MISSING;
    assert(*size >= history_sizes[k]);
    *size = history_sizes[k];
    memcpy(out, history_blobs[k], *size);
    return RTC_HISTORY_STORE_VALID;
}
static bool history_write(void *u, const char *key, const void *in, size_t size) {
    (void)u;
    ++history_writes;
    if (history_fail) return false;
    int k = history_key(key);
    memcpy(history_blobs[k], in, size);
    history_sizes[k] = size;
    return true;
}
static bool history_commit(void *u) { (void)u; ++history_commits; return !history_fail; }
rtc_history_store_result_t rtc_history_store_platform_io(rtc_history_store_io_t *io) {
    ++history_init_calls;
    memset(io, 0, sizeof *io);
    if (history_init_fail) return RTC_HISTORY_STORE_IO_ERROR;
    *io = (rtc_history_store_io_t){NULL, history_read, history_write, history_commit};
    return RTC_HISTORY_STORE_VALID;
}
uint64_t rtc_clock_platform_history_epoch(void) { return UINT64_C(1234) + ++epoch_calls; }

#include "../main/rtc_clock.c"

/* 模拟启动时普通 RAM 清零，RTC 保留对象不随深睡重启清零。 */
static void reboot_runtime(void) { free(s_clock.history); memset(&s_clock, 0, sizeof s_clock); }
static void reset_all(void) {
    reboot_runtime();
    memset(&rtc_clock_retained, 0, sizeof rtc_clock_retained);
    memset(history_blobs, 0, sizeof history_blobs);
    memset(history_sizes, 0, sizeof history_sizes);
    history_reads = history_writes = history_commits = epoch_calls = 0;
    history_fail = history_ack_loss = history_init_fail = false;
    history_init_calls = 0;
    ticks = 100; rtc_us = 0; uptime_us = 1000000;
    wall_us = INT64_C(1700000000123456); applied_us = 0;
    period = 3U * RTC_DRIFT_Q19; temperature = 25000;
    fail_set = fail_get = fail_sensor = fail_stop = fail_save = false;
    sets = saves = stops = reads = measures = delayed_tick_reads = 0;
    tick_delay_before_us = tick_delay_after_us = set_delay_us = 0;
    wall_delay_before_us = wall_delay_after_us = 0;
    store_state = RTC_CLOCK_STORE_MISSING;
    coefficient_store = RTC_CLOCK_STORE_MISSING;
    coefficient_saves = source = 0;
    stored_ppm = 0;
    fail_coefficient_save = change_source_on_tick = false;
    memset(&light_snapshot, 0, sizeof light_snapshot);
    light_period = 0;
    light_inhibited = light_start_fail = false;
    memset(&stored_model, 0, sizeof stored_model);
}
static int64_t prepare_session(void) {
    assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
    rtc_clock_tick();
    assert(rtc_clock_prepare_sleep());
    assert(rtc_clock_commit_sleep(true));
    assert(rtc_clock_session_valid(&rtc_clock_retained.session));
    assert(stops == 1);
    return wall_us;
}
static void check_absolute_bridge_and_consume(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        reset_all();
        const int64_t anchor = prepare_session();
        ticks += 1000;
        wall_us += sign * INT64_C(9999999);
        uptime_us = 50000;
        rtc_us = 3000;
        const uint64_t security_clock = rtc_us;
        reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        assert(applied_us == anchor + 3000 && sets == 1);
        const rtc_clock_diagnostics_t diagnostic = rtc_clock_diagnostics();
        assert(diagnostic.reconciled_elapsed_us == 3000);
        assert(diagnostic.sdk_sample_valid);
        assert(diagnostic.sdk_correction_us == 3000 - sign * INT64_C(9999999));
        assert(rtc_us == security_clock && uptime_us == 50000);
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED && sets == 1);
        assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_CONSUMED);
    }
}
static void check_failed_apply_retry_and_cold(void) {
    reset_all();
    const int64_t anchor = prepare_session();
    ticks += 1000;
    reboot_runtime(); fail_set = true;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC);
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_ACTIVE);
    ticks += 20; fail_set = false;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(applied_us == anchor + 3060 && sets == 2);
    reboot_runtime();
    assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
    assert(sets == 2 && !rtc_clock_retained.session.magic);
}
static void check_corrupt_state_and_external_sync(void) {
    reset_all(); prepare_session();
    rtc_clock_retained.session.ledger.period_q19 = 0;
    rtc_clock_session_seal(&rtc_clock_retained.session);
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && sets == 0);
    rtc_clock_wall_time_set();
    assert(!rtc_clock_retained.session.magic);
    reset_all(); prepare_session();
    rtc_clock_retained.session.utc_anchor_us ^= 1;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && sets == 0);
}
static void check_sampling_and_shutdown_failure(void) {
    reset_all();
    assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
    rtc_clock_tick();
    const unsigned count = reads;
    rtc_clock_tick();
    assert(reads == count && saves == 0);
    fail_stop = true;
    assert(!rtc_clock_prepare_sleep());
    assert(!rtc_clock_retained.session.magic);
    fail_stop = false;
    assert(rtc_clock_prepare_sleep());
    assert(rtc_clock_commit_sleep(false));
    ticks += 1000;
    reboot_runtime();
    assert(rtc_clock_boot(true, false) == RTC_CLOCK_BASELINE && sets == 0);
}
static void feed_training(void) {
    for (int i = 0; i < 12; ++i) {
        temperature = 10000 + i * 1000;
        rtc_clock_tick();
        uptime_us += 60000000;
        rtc_us += 60000000;
    }
}
static void check_storage_budget_and_failure(void) {
    reset_all(); rtc_clock_boot(false, false);
    feed_training();
    assert(rtc_drift_model_validate(&rtc_clock_retained.model));
    assert(saves == 1 && store_state == RTC_CLOCK_STORE_VALID);
    feed_training();
    assert(saves == 1);
    reboot_runtime(); rtc_clock_boot(false, false);
    period += 1000;
    feed_training();
    assert(saves == 1);
    reset_all(); fail_save = true; rtc_clock_boot(false, false);
    feed_training();
    assert(saves == 1 && store_state == RTC_CLOCK_STORE_MISSING);
    feed_training();
    assert(saves == 1);
    reset_all(); store_state = RTC_CLOCK_STORE_BLOCKED;
    rtc_clock_boot(false, false); feed_training();
    assert(saves == 0);
}
static void check_capture_skew_and_retry(void) {
    /* RTC 读取前后都可能被调度；过宽窗口必须重采样，不能成功消费错误相位。 */
    for (unsigned before = 0; before < 2; ++before) {
        reset_all();
        const int64_t anchor = prepare_session();
        ticks += 1000;
        reboot_runtime();
        delayed_tick_reads = 1;
        if (before) tick_delay_before_us = 99999;
        else tick_delay_after_us = 99999;
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        assert(applied_us == anchor + 3000 + 99999);
    }
    reset_all();
    prepare_session(); reboot_runtime();
    delayed_tick_reads = 2; tick_delay_after_us = 99999;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && sets == 0);
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_ACTIVE);
    reset_all();
    prepare_session(); reboot_runtime();
    set_delay_us = 99999;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC);
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_CONSUMED);
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && sets == 1);
    reset_all();
    rtc_clock_boot(false, false); rtc_clock_tick(); assert(rtc_clock_prepare_sleep());
    delayed_tick_reads = 1; tick_delay_after_us = 99999;
    assert(rtc_clock_commit_sleep(true));
    const int64_t paired = wall_us;
    ticks += 1000; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED && applied_us == paired + 3000);
}
static void check_rejected_old_period_is_not_restored(void) {
    reset_all(); rtc_clock_boot(false, false); rtc_clock_tick();
    uptime_us += 60000000; temperature = 80000; period = 0;
    assert(rtc_clock_prepare_sleep());
    assert(rtc_clock_commit_sleep(true));
    assert(!rtc_clock_retained.session.magic);
}
static void check_maintenance_fault_latch(void) {
    reset_all(); prepare_session();
    rtc_clock_retained.maintenance_blocked = 1;
    assert(rtc_clock_abort_sleep());
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(rtc_clock_retained.session.maintenance_enabled == 0);
    reboot_runtime();
    rtc_clock_boot(true, true);
    assert(rtc_clock_retained.maintenance_blocked == 1);
    reboot_runtime();
    rtc_clock_boot(false, false);
    assert(rtc_clock_retained.maintenance_blocked == 0);
}
static void check_read_only_diagnostics(void) {
    reset_all(); prepare_session();
    rtc_clock_retained.session.samples = 2;
    rtc_clock_retained.session.user_wake = 1;
    rtc_clock_retained.session.pre_tail_gap_upper_us = 120;
    rtc_clock_session_seal(&rtc_clock_retained.session);
    ticks += 1000; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    unsigned old_reads = reads, old_sets = sets, old_saves = saves;
    rtc_clock_diagnostics_t d = rtc_clock_diagnostics();
    assert(d.utc_restored && d.user_wake && d.samples == 2);
    assert(d.period_q19 == 3 * RTC_DRIFT_Q19 && d.pre_tail_gap_upper_us == 120);
    assert(!d.maintenance_blocked && !d.model_qualified);
    assert(reads == old_reads && sets == old_sets && saves == old_saves);
}

static void check_sleep_rate_requires_new_measurement(void) {
    for (unsigned predicted = 0; predicted < 2; ++predicted) {
        reset_all();
        assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
        rtc_clock_tick();
        rtc_clock_retained.model = (rtc_drift_model_t){
            .coeff_q19 = {3 * RTC_DRIFT_Q19, 8192, 0}, .rms_ppm = 0,
            .min_temp_mc = 25000, .max_temp_mc = 30000,
            .observations = 12, .occupied_bins = 3, .qualified = true
        };
        assert(rtc_drift_model_validate(&rtc_clock_retained.model));
        if (predicted) uptime_us += 3000000;
        temperature = predicted ? 30000 : 25000;
        period += 1000;
        const unsigned before = measures;
        assert(rtc_clock_prepare_sleep());
        assert(measures == before + 1);
        assert(rtc_clock_commit_sleep(true));
        assert(rtc_clock_retained.session.ledger.period_q19 == period);
    }
    /* 新测量失败时不能以拟合或旧缓存冒充成功的睡前校准。 */
    reset_all(); rtc_clock_boot(false, false); rtc_clock_tick();
    period = 0;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(!rtc_clock_retained.session.magic);
}

static void check_optional_sdk_diagnostic(void) {
    reset_all(); prepare_session();
    const int64_t anchor = wall_us;
    ticks += 1000; uptime_us = 50000;
    reboot_runtime(); fail_get = true;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(applied_us == anchor + 3000);
    rtc_clock_diagnostics_t diagnostic = rtc_clock_diagnostics();
    assert(diagnostic.reconciled_elapsed_us == 3000 && !diagnostic.sdk_sample_valid);
    assert(diagnostic.sdk_correction_us == 0);
    rtc_clock_wall_time_set();
    diagnostic = rtc_clock_diagnostics();
    assert(!diagnostic.utc_restored && !diagnostic.sdk_sample_valid);
    assert(diagnostic.reconciled_elapsed_us == 0 && diagnostic.sdk_correction_us == 0);
}

static void check_sdk_diagnostic_phase_and_slow_sample(void) {
    const int64_t delays[] = {300, 1500};
    for (unsigned i = 0; i < sizeof delays / sizeof delays[0]; ++i) {
        reset_all();
        const int64_t anchor = prepare_session();
        ticks += 1000; wall_us += 5000; uptime_us = 50000;
        reboot_runtime();
        wall_delay_before_us = wall_delay_after_us = delays[i];
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        assert(applied_us == anchor + 3000 + 2 * delays[i]);
        const rtc_clock_diagnostics_t diagnostic = rtc_clock_diagnostics();
        assert(diagnostic.utc_restored && diagnostic.reconciled_elapsed_us == 3000);
        assert(diagnostic.sdk_sample_valid == (i == 0));
        assert(diagnostic.sdk_correction_us == (i == 0 ? -2000 : 0));
    }
}

static void check_saved_coefficient_fractional_chain(void) {
    const int32_t rates[] = {-20000, -1234, 1234, 20000};
    for (unsigned j = 0; j < sizeof rates / sizeof rates[0]; ++j) {
        reset_all();
        coefficient_store = RTC_CLOCK_STORE_VALID;
        stored_ppm = rates[j];
        source = j % 2 ? 2 : 0;
        period += 197;
        const int64_t anchor = prepare_session();
        rtc_drift_time_t total = {0}, corrected_total = {0};
        rtc_drift_ratio_t ratio = {0};
        for (unsigned i = 0; i < 96; ++i) {
            const uint64_t dt = 10001 + i;
            rtc_drift_time_t raw = {
                .us = dt * period / RTC_DRIFT_Q19,
                .fraction_q19 = (uint32_t)(dt * period % RTC_DRIFT_Q19)
            }, corrected;
            assert(rtc_drift_time_add(&total, &raw, &total));
            assert(rtc_drift_scale_time(&raw, rates[j], &ratio, &corrected));
            assert(rtc_drift_time_add(&corrected_total, &corrected, &corrected_total));
            ticks += dt;
            uptime_us = 20000;
            reboot_runtime();
            assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
            assert(applied_us == anchor + (int64_t)corrected_total.us);
            assert(rtc_clock_retained.accounting.raw_total.us == total.us);
            assert(rtc_clock_retained.accounting.raw_total.fraction_q19 == total.fraction_q19);
            assert(rtc_clock_retained.accounting.corrected_total.us == corrected_total.us);
            assert(rtc_clock_retained.accounting.corrected_total.fraction_q19 == corrected_total.fraction_q19);
            assert(rtc_clock_retained.accounting.ratio.denominator == ratio.denominator);
            assert(rtc_clock_retained.accounting.ratio.remainder == ratio.remainder);
            const rtc_clock_diagnostics_t d = rtc_clock_diagnostics();
            assert(d.ppm == rates[j] && d.ppm_qualified && d.source == source);
            assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        }
        assert(coefficient_saves == 0);
    }
}

static void check_maintenance_abort_and_awake(void) {
    reset_all();
    const int64_t anchor = prepare_session();
    ticks += 100000;
    uptime_us = 4000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(!rtc_clock_maintenance_due(false) && rtc_clock_maintenance_due(true));
    const unsigned old_reads = reads, old_stops = stops, old_saves = saves;
    advance_fake(6000);
    assert(rtc_clock_prepare_maintenance());
    assert(reads == old_reads && stops == old_stops && saves == old_saves && !coefficient_saves);
    assert(rtc_clock_commit_sleep(true));
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_ACTIVE);
    assert(rtc_clock_retained.accounting.maintenance_count == 1);
    advance_fake(9000);
    assert(rtc_clock_abort_sleep());
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_CONSUMED);
    assert(rtc_clock_retained.accounting.raw_total.us == 300000);
    assert(rtc_clock_retained.accounting.maintenance_count == 0);
    assert(!rtc_clock_maintenance_due(true));
    advance_fake(3000);
    rtc_clock_begin_normal_wake();
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(rtc_clock_retained.accounting.awake_total_us == 18000);
    assert(wall_us == anchor + 318000);
    ticks += 1000;
    uptime_us = 2000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(applied_us == anchor + 321000);
    rtc_clock_diagnostics_t d = rtc_clock_diagnostics();
    assert(d.raw_chain_us == 303000 && d.corrected_chain_us == 303000);
    assert(d.awake_total_us == 18000 && d.maintenance_count == 0);
    assert(rtc_clock_abort_sleep() && rtc_clock_abort_sleep());
}

static rtc_clock_reference_t reference_now(uint64_t id, uint64_t utc) {
    return (rtc_clock_reference_t){id, utc, 1000, uptime_us, true};
}

static void check_reference_transaction_and_coordinates(void) {
    reset_all();
    coefficient_store = RTC_CLOCK_STORE_VALID;
    stored_ppm = 1000;
    const uint64_t anchor = (uint64_t)prepare_session();
    ticks += UINT64_C(7207200000);
    uptime_us = 10000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    const rtc_drift_time_t raw = rtc_clock_retained.accounting.raw_total;
    advance_fake(100000);
    rtc_clock_reference_t ref = reference_now(100, anchor + UINT64_C(21600000000) + 70000);
    ref.hrt_us -= 30000;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(applied_us == (int64_t)ref.utc_us + 30000);
    rtc_drift_learning_t learned = rtc_clock_retained.accounting.learning;
    assert(learned.baseline_valid && learned.baseline.raw.us == raw.us);
    assert(learned.baseline.awake_us == 70000 && learned.baseline.utc_us == ref.utc_us);
    assert(learned.ppm == 1000 && learned.qualified);
    assert(!rtc_clock_retained.session.magic && !rtc_clock_maintenance_due(true));
    advance_fake(50000);
    ref = reference_now(101, ref.utc_us + 80000);
    fail_set = true;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_FAILED);
    assert(!memcmp(&learned, &rtc_clock_retained.accounting.learning, sizeof learned));
    fail_set = false;
    ref.quality = false;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_retained.accounting.learning.ppm == 1000);
    ref = reference_now(102, ref.utc_us + 1000);
    set_delay_us = 2000;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_UNCERTAIN);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_diagnostics().reference_status == RTC_CLOCK_REFERENCE_UNCERTAIN);
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC);
    set_delay_us = 0;
    ref = reference_now(103, ref.utc_us + 1000);
    ref.hrt_us++;
    const unsigned old_sets = sets;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_FAILED && sets == old_sets);
    ref.hrt_us = uptime_us;
    ref.utc_us = 1577836800ULL * 1000000 - 1;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_FAILED && sets == old_sets);
    ref.utc_us = 4102444800ULL * 1000000 + 1;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_FAILED && sets == old_sets);
    ref = reference_now(104, anchor);
    advance_fake(5000001);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    rtc_clock_wall_time_set();
    assert(!rtc_clock_retained.accounting.utc_phase_q19);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_retained.accounting.learning.ppm == 1000);
}

static void check_reference_before_aborted_commit(void) {
    reset_all();
    rtc_clock_boot(false, false);
    advance_fake(10000);
    rtc_clock_reference_t ref = reference_now(11, (uint64_t)wall_us);
    advance_fake(20000);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    advance_fake(20000);
    assert(rtc_clock_abort_sleep());
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_retained.accounting.learning.baseline.awake_us == 10000);
    assert(rtc_clock_retained.accounting.learning.baseline.raw.us == 0);
}

static void check_discontinuities(void) {
    for (unsigned kind = 0; kind < 6; ++kind) {
        reset_all(); prepare_session();
        if (kind == 0) source = 2;
        if (kind == 1) rtc_clock_retained.accounting.awake_total_us ^= 1;
        if (kind == 2) rtc_clock_retained.session.checksum ^= 1;
        if (kind == 3) rtc_clock_retained.accounting.version = 0;
        if (kind == 4) memset(&rtc_clock_retained.session, 0, sizeof rtc_clock_retained.session);
        if (kind == 5) change_source_on_tick = true;
        ticks += 1000; uptime_us = 100;
        reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && sets == 0);
        assert(!rtc_clock_maintenance_due(true));
        assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    }
}

static void check_untrusted_restore_breaks_learning(void) {
    reset_all();
    rtc_clock_boot(false, false);
    rtc_clock_reference_t ref = reference_now(10, (uint64_t)wall_us);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000;
    uptime_us = 1000;
    reboot_runtime();
    assert(rtc_clock_boot(true, false) == RTC_CLOCK_BASELINE && sets == 1);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_CONSUMED);
}

static void check_learning_and_coefficient_budget(void) {
    reset_all(); rtc_clock_boot(false, false);
    uint64_t utc = (uint64_t)wall_us;
    rtc_clock_reference_t ref = reference_now(1, utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    for (unsigned i = 0; i < 2; ++i) {
        advance_fake(60000);
        utc += 60000;
        assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        ticks += UINT64_C(7214400000);
        utc += UINT64_C(21600000000);
        uptime_us = 3000;
        reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        advance_fake(90000);
        utc += 90000;
        ref = reference_now(i + 2, utc);
        ref.hrt_us -= 30000;
        ref.utc_us -= 30000;
        assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    }
    rtc_clock_diagnostics_t d = rtc_clock_diagnostics();
    assert(!d.ppm_qualified && d.ppm == 0 && d.learning_reason == RTC_DRIFT_LEARN_PUBLISHED);
    assert(rtc_clock_retained.accounting.learning.ppm == 2000 && rtc_clock_retained.accounting.learning.qualified);
    rtc_clock_tick();
    assert(coefficient_saves == 0);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += UINT64_C(14428800000);
    uptime_us = 3000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_maintenance_due(true) && coefficient_saves == 0);
    const unsigned old_reads = reads, old_saves = saves, old_measures = measures;
    assert(rtc_clock_prepare_maintenance());
    rtc_clock_tick();
    assert(rtc_clock_commit_sleep(true));
    assert(reads == old_reads && saves == old_saves && measures == old_measures + 1);
    assert(coefficient_saves == 0);
    assert(rtc_clock_abort_sleep());
    fail_coefficient_save = true;
    rtc_clock_tick();
    assert(coefficient_saves == 0);
    rtc_clock_tick();
    assert(coefficient_saves == 0);
    advance_fake(INT64_C(86400) * 1000000 - 1);
    rtc_clock_tick();
    assert(coefficient_saves == 0);
    advance_fake(60000001);
    fail_coefficient_save = false;
    rtc_clock_tick();
    assert(coefficient_saves == 0 && stored_ppm == 0);
    advance_fake(INT64_C(86400) * 1000000);
    rtc_clock_tick();
    assert(coefficient_saves == 0);
    reboot_runtime();
    assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
    assert(!rtc_clock_diagnostics().ppm_qualified && rtc_clock_diagnostics().ppm == 0);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
}

static void check_saved_coefficient_significance(void) {
    for (int32_t rate = 2009; rate <= 2010; ++rate) {
        reset_all();
        coefficient_store = RTC_CLOCK_STORE_VALID;
        stored_ppm = 2000;
        rtc_clock_boot(false, false);
        uint64_t utc = (uint64_t)wall_us;
        rtc_clock_reference_t ref = reference_now(UINT64_MAX, utc);
        assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
        for (unsigned i = 0; i < 2; ++i) {
            assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
            assert(rtc_clock_retained.session.ppm == 2000);
            ticks += (UINT64_C(21600000000) * (uint32_t)(1000000 + rate) / 1000000) / 3;
            uptime_us = 3000;
            reboot_runtime();
            assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
            utc += UINT64_C(21600000000);
            ref = reference_now(UINT64_MAX - i - 1, utc);
            assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
        }
        assert(rtc_clock_diagnostics().ppm == 2000 && rtc_clock_diagnostics().ppm_qualified);
        assert(rtc_clock_retained.accounting.learning.ppm == rate);
        assert(rtc_clock_retained.accounting.learning.observations == 2);
        rtc_clock_tick();
        assert(coefficient_saves == 0);
        advance_fake(INT64_C(12) * 3600 * 1000000);
        source = 2;
        const unsigned old_reads = reads;
        rtc_clock_tick();
        assert(coefficient_saves == 0 && reads == old_reads);
        source = 0;
        rtc_clock_tick();
        assert(coefficient_saves == 0);
        assert(stored_ppm == 2000);
        assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        assert(rtc_clock_retained.session.ppm == 2000);
    }
    reset_all();
    coefficient_store = RTC_CLOCK_STORE_VALID;
    stored_ppm = 20001;
    rtc_clock_boot(false, false);
    assert(!rtc_clock_diagnostics().ppm_qualified && !rtc_clock_diagnostics().ppm);
}

static void check_late_reference_does_not_publish_candidate(void) {
    reset_all(); rtc_clock_boot(false, false);
    uint64_t utc = (uint64_t)wall_us;
    rtc_clock_reference_t ref = reference_now(100, utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    for (unsigned i = 0; i < 2; ++i) {
        assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        ticks += UINT64_C(7214400000);
        utc += UINT64_C(21600000000);
        uptime_us = 3000; reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        ref = reference_now(101 + i, utc);
        if (i == 1) set_delay_us = 2001;
        assert(rtc_clock_apply_reference(&ref) ==
            (i == 1 ? RTC_CLOCK_REFERENCE_UNCERTAIN : RTC_CLOCK_REFERENCE_SET));
    }
    assert(!rtc_clock_diagnostics().ppm_qualified && rtc_clock_diagnostics().ppm == 0);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid && !coefficient_saves);
}

static void check_maintenance_diagnostics_and_measurement_fault(void) {
    reset_all(); prepare_session();
    ticks += 1000; uptime_us = 100; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_prepare_maintenance() && rtc_clock_commit_sleep(true));
    ticks += 1000; uptime_us = 100; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_diagnostics().maintenance_count == 1);
    rtc_clock_begin_normal_wake();
    assert(rtc_clock_diagnostics().maintenance_count == 1);
    period = 0;
    assert(!rtc_clock_prepare_maintenance());
    assert(rtc_clock_diagnostics().maintenance_blocked);
    assert(rtc_clock_diagnostics().fault == RTC_CLOCK_FAULT_CALIBRATION);
    assert(rtc_clock_abort_sleep());
    period = 3 * RTC_DRIFT_Q19;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(!rtc_clock_retained.session.maintenance_enabled);
}

static void check_source_model_isolation_and_capture_uncertainty(void) {
    reset_all(); rtc_clock_boot(false, false); rtc_clock_tick();
    source = 2;
    const unsigned old_reads = reads, old_saves = saves, old_measures = measures;
    assert(!rtc_clock_prepare_sleep());
    assert(reads == old_reads && saves == old_saves && measures == old_measures);
    reset_all(); prepare_session();
    rtc_clock_retained.model = (rtc_drift_model_t){
        .coeff_q19 = {3 * RTC_DRIFT_Q19, 8192, 0}, .rms_ppm = 0,
        .min_temp_mc = 25000, .max_temp_mc = 30000,
        .observations = 12, .occupied_bins = 3, .qualified = true
    };
    rtc_clock_retained.training.count = 1;
    source = 2; ticks += 1000; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC);
    assert(!rtc_clock_retained.model.qualified && !rtc_clock_retained.training.count);
    reset_all(); rtc_clock_boot(false, false);
    rtc_clock_reference_t ref = reference_now(1, (uint64_t)wall_us);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_retained.accounting.learning.baseline.uncertainty_us == 1000);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000; uptime_us = 100; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_retained.accounting.learning.baseline.uncertainty_us >= 3000);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000; uptime_us = 100; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_retained.accounting.learning.baseline.uncertainty_us >= 5000);
}

static void check_corrupt_boolean_is_rejected_before_decode(void) {
    reset_all(); prepare_session();
    unsigned char *qualified = (unsigned char *)&rtc_clock_retained.accounting.learning.qualified;
    qualified[0] = 2;
    ticks += 1000; uptime_us = 3000; reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && !sets);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
}

static void check_unresolved_bridge_cannot_train(void) {
    reset_all(); rtc_clock_boot(false, false);
    uint64_t utc = (uint64_t)wall_us;
    rtc_clock_reference_t ref = reference_now(100, utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    for (unsigned i = 0; i < 2; ++i) {
        assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        ticks += UINT64_C(7200000000);
        utc += UINT64_C(21600000000);
        uptime_us = 3000; reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        if (!i) {
            ref = reference_now(101, utc);
            assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
            assert(rtc_clock_retained.accounting.learning.pending);
        }
    }
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 333333; utc += 999999;
    uptime_us = 3000; reboot_runtime(); fail_set = true;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_NEEDS_SYNC && !s_clock.boot_done);
    assert(rtc_clock_retained.session.ledger.phase == RTC_DRIFT_LEDGER_ACTIVE);
    fail_set = false;
    ref = reference_now(102, utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(!rtc_clock_diagnostics().ppm_qualified && rtc_clock_diagnostics().ppm == 0);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(!rtc_clock_retained.session.magic);
}

static void check_awake_only_reference_transaction(void) {
    for (unsigned mode = 0; mode < 6; ++mode) {
        reset_all();
        coefficient_store = RTC_CLOCK_STORE_VALID;
        stored_ppm = -870;
        assert(rtc_clock_boot(false, false) == RTC_CLOCK_BASELINE);
        uint64_t utc = (uint64_t)wall_us;
        rtc_clock_reference_t ref = reference_now(100, utc);
        assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
        assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
        ticks += UINT64_C(7214400000);
        utc += UINT64_C(21600000000);
        uptime_us = 3000; reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        ref = reference_now(101, utc);
        assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
        assert(rtc_clock_retained.accounting.learning.pending);
        rtc_drift_learning_t expected = rtc_clock_retained.accounting.learning;
        const rtc_drift_time_t raw = rtc_clock_retained.accounting.raw_total;
        const rtc_drift_time_t corrected = rtc_clock_retained.accounting.corrected_total;
        const uint64_t awake = rtc_clock_diagnostics().awake_total_us;
        for (unsigned i = 0; i < 3; ++i) {
            advance_fake(100000);
            utc += 100000;
            ref = reference_now(102 + i, utc);
            if (mode == 1) fail_set = true;
            if (mode == 2) ref.quality = false;
            if (mode == 3) set_delay_us = 2001;
            if (mode == 4) advance_fake(5000001);
            if (mode == 5) source = 2;
            const unsigned old_sets = sets;
            assert(rtc_clock_apply_reference(&ref) == (mode == 1 ? RTC_CLOCK_REFERENCE_FAILED :
                mode == 3 ? RTC_CLOCK_REFERENCE_UNCERTAIN : RTC_CLOCK_REFERENCE_SET));
            assert(sets == old_sets + 1);
            assert(applied_us == (int64_t)ref.utc_us + (mode == 4 ? 5000001 : 0) || mode == 1);
            assert(rtc_clock_retained.accounting.raw_total.us == raw.us);
            assert(rtc_clock_retained.accounting.raw_total.fraction_q19 == raw.fraction_q19);
            assert(rtc_clock_retained.accounting.corrected_total.us == corrected.us);
            assert(rtc_clock_retained.accounting.corrected_total.fraction_q19 == corrected.fraction_q19);
            assert(rtc_clock_diagnostics().awake_total_us == awake + (i + 1) * UINT64_C(100000) +
                (mode == 3 ? 2001 : mode == 4 ? 5000001 : 0));
            assert(rtc_clock_diagnostics().ppm == -870 && rtc_clock_diagnostics().ppm_qualified);
            if (mode == 1) {
                assert(!memcmp(&expected, &rtc_clock_retained.accounting.learning, sizeof expected));
                fail_set = false;
                assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
            } else if (mode != 0) {
                assert(!rtc_clock_retained.accounting.learning.baseline_valid);
                assert(!rtc_clock_retained.accounting.learning.pending);
                break;
            }
            expected.last_reference_id = ref.id;
            expected.last_seen_utc_us = ref.utc_us;
            assert(!memcmp(&expected, &rtc_clock_retained.accounting.learning, sizeof expected));
            assert(rtc_clock_diagnostics().learning_reason == RTC_DRIFT_LEARN_NO_NEW_SLEEP);
        }
        if (mode <= 1) {
            assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
            ticks += UINT64_C(7214400000);
            utc += UINT64_C(21600000000);
            uptime_us = 3000; reboot_runtime();
            assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
            ref = reference_now(105, utc);
            assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
            assert(rtc_clock_diagnostics().learning_reason == RTC_DRIFT_LEARN_PUBLISHED);
            assert(rtc_clock_diagnostics().ppm == -870);
            assert(rtc_clock_retained.accounting.learning.ppm == 2000);
            assert(rtc_clock_retained.accounting.learning.observations == 2);
        }
    }
}

static uint64_t history_test_utc;
static uint64_t history_test_id;
static void history_test_start(void) {
    reset_all();
    coefficient_store = RTC_CLOCK_STORE_VALID;
    stored_ppm = -870;
    rtc_clock_boot(false, true);
    rtc_clock_begin_normal_wake();
    history_test_utc = (uint64_t)wall_us;
    history_test_id = 1;
    rtc_clock_reference_t ref = reference_now(history_test_id, history_test_utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
}
static void history_test_restore(void) {
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += UINT64_C(7214400000);
    history_test_utc += UINT64_C(21600000000);
    uptime_us = 3000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    rtc_clock_begin_normal_wake();
}
static void history_test_window(void) {
    history_test_restore();
    rtc_clock_reference_t ref = reference_now(++history_test_id, history_test_utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
}
static void check_history_capture_transaction(void) {
    history_test_start();
    history_test_window();
    rtc_clock_history_summary_t summary = rtc_clock_history_owner_summary();
    assert(summary.accepted_sequence == 1 && summary.pending_count == 1);
    assert(summary.durable_count == 0 && summary.active_ppm == -870);
    rtc_clock_reference_t ref = reference_now(++history_test_id, history_test_utc + 1000);
    advance_fake(1000);
    ref.hrt_us = uptime_us;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    summary = rtc_clock_history_owner_summary();
    assert(summary.accepted_sequence == 1 && summary.pending_count == 1);
    rtc_clock_tick();
    summary = rtc_clock_history_owner_summary();
    assert(summary.durable_count == 1 && summary.pending_count == 0 && summary.durable_sequence == 1);
    rtc_history_page_t page;
    assert(rtc_clock_history_owner_page(summary.generation, 0, &page));
    assert(page.count == 1 && page.rows[0].raw.us == UINT64_C(21643200000));
    assert(page.rows[0].combined_uncertainty_us == 4000);
    assert(!rtc_clock_history_owner_page(summary.generation + 1, 0, &page));
}

static void check_history_queue_and_cold(void) {
    history_test_start();
    history_test_window();
    history_test_window();
    rtc_clock_history_retained_t pending = rtc_clock_retained.history;
    rtc_drift_learning_t learner = rtc_clock_retained.accounting.learning;
    history_test_window();
    history_test_window();
    rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
    assert(view.pending_count == 2 && view.accepted_sequence == 4 && view.dropped == 2);
    assert(!memcmp(pending.rows, rtc_clock_retained.history.rows, sizeof pending.rows));
    assert(learner.ppm == rtc_clock_retained.accounting.learning.ppm);
    assert(learner.observations == rtc_clock_retained.accounting.learning.observations);
    assert(rtc_clock_retained.accounting.learning.baseline.utc_us == history_test_utc);
    assert(view.active_ppm == -870 && stored_ppm == -870);
    rtc_clock_tick();
    view = rtc_clock_history_owner_summary();
    assert(view.pending_count == 1 && view.durable_count == 1 && view.durable_sequence == 1);
    const unsigned commits = history_commits;
    rtc_clock_tick();
    assert(history_commits == commits);
    reboot_runtime();
    rtc_clock_boot(false, true);
    assert(epoch_calls == 1);
    rtc_clock_begin_normal_wake();
    view = rtc_clock_history_owner_summary();
    assert(view.pending_count == 0 && view.durable_count == 1);
    assert(view.accepted_sequence == 4 && view.durable_sequence == 1 && view.dropped == 2);
    assert(view.flags & 1);
    assert(epoch_calls == 2 && view.save_budget_us == 0);
    assert(view.active_ppm == -870 && !rtc_clock_retained.accounting.learning.baseline_valid);
}

static void check_history_corruption(void) {
    history_test_start();
    history_test_window();
    assert(rtc_clock_retained.accounting.learning.baseline_valid);
    rtc_clock_retained.history.rows[0][0] ^= 1;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000;
    uptime_us = 3000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    rtc_clock_begin_normal_wake();
    rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
    assert(view.pending_count == 0 && (view.flags & 1));
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    assert(view.active_ppm == -870);
}

static void check_history_persistence_failures(void) {
    for (unsigned ack = 0; ack < 2; ++ack) {
        history_test_start();
        history_test_window();
        if (ack) history_ack_loss = true;
        else history_fail = true;
        rtc_clock_tick();
        rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
        assert(view.pending_count == 1 && view.save_budget_us == 0);
        assert(view.store_status == RTC_HISTORY_STORE_IO_ERROR);
        const unsigned writes = history_writes, io = history_reads;
        history_fail = false;
        rtc_clock_tick();
        assert(history_writes == writes && history_reads == io);
        advance_fake(INT64_C(21600000000));
        rtc_clock_tick();
        view = rtc_clock_history_owner_summary();
        assert(view.pending_count == 0 && view.durable_count == 1 && view.durable_sequence == 1);
        if (ack) assert(history_writes == writes);
        assert(view.active_ppm == -870 && stored_ppm == -870);
    }
}

static void check_history_reboot_after_manifest_commit(void) {
    history_test_start();
    history_test_window();
    history_ack_loss = true;
    rtc_clock_tick();
    assert(rtc_clock_history_owner_summary().store_status == RTC_HISTORY_STORE_IO_ERROR);
    assert(rtc_clock_history_owner_summary().pending_count == 1);
    const unsigned writes = history_writes;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000;
    uptime_us = 3000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    rtc_clock_begin_normal_wake();
    rtc_clock_tick();
    rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
    assert(view.ready && view.durable_count == 1 && view.pending_count == 0);
    assert(view.accepted_sequence == 1 && view.durable_sequence == 1);
    assert(view.generation == 1 && history_writes == writes);
    assert(view.active_ppm == -870 && stored_ppm == -870);
}

static void check_history_setter_environment(void) {
    history_test_start();
    history_test_restore();
    rtc_clock_reference_t ref = reference_now(++history_test_id, history_test_utc);
    rtc_clock_accounting_t a = rtc_clock_retained.accounting;
    rtc_clock_history_retained_t h = rtc_clock_retained.history;
    fail_set = true;
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_FAILED);
    assert(!memcmp(&a, &rtc_clock_retained.accounting, sizeof a));
    assert(!memcmp(&h, &rtc_clock_retained.history, sizeof h));
    fail_set = false;
    read_temperature(uptime_us);
    rtc_clock_battery_cache_update(true, 3900, uptime_us);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    rtc_history_record_t row;
    assert(rtc_history_decode(rtc_clock_retained.history.rows[0], 80, &row));
    assert(row.flags == (RTC_HISTORY_TEMP_VALID | RTC_HISTORY_BATTERY_VALID | RTC_HISTORY_AWAKE_ENDPOINT_ONLY));
    assert(row.temperature_centi_c == 2500 && row.battery_mv == 3900);
    assert(!row.temp_age_s && !row.battery_age_s);
    rtc_clock_wall_time_set();
    assert(rtc_clock_retained.history.count == 1);
    assert(rtc_clock_diagnostics().ppm == -870);
    history_test_start();
    history_test_restore();
    read_temperature(uptime_us);
    rtc_clock_battery_cache_update(true, 3900, uptime_us);
    advance_fake(INT64_C(121000000));
    history_test_utc += UINT64_C(121000000);
    ref = reference_now(++history_test_id, history_test_utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_history_decode(rtc_clock_retained.history.rows[0], 80, &row));
    assert(row.flags == 0 && row.battery_mv == 0 && row.temperature_centi_c == 0);
}

static void check_history_maintenance_io_isolation(void) {
    history_test_start();
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000;
    uptime_us = 3000;
    reboot_runtime();
    const unsigned io = history_reads, writes = history_writes, nonces = epoch_calls;
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    assert(rtc_clock_prepare_maintenance());
    rtc_clock_tick();
    assert(rtc_clock_commit_sleep(true));
    assert(history_reads == io && history_writes == writes && epoch_calls == nonces);
    assert(!rtc_clock_history_owner_summary().ready);
    assert(rtc_clock_abort_sleep());
    rtc_clock_begin_normal_wake();
    assert(history_reads > io && rtc_clock_history_owner_summary().ready);
}

static void check_shadow_active_isolation(void) {
    reset_all();
    coefficient_store = RTC_CLOCK_STORE_VALID;
    stored_ppm = -870;
    rtc_clock_boot(false, true);
    rtc_clock_retained.accounting.learning.ppm = 2000;
    rtc_clock_retained.accounting.learning.qualified = true;
    rtc_clock_retained.accounting.coefficient_budget_us = RTC_CLOCK_SAVE_INTERVAL_US;
    seal_accounting();
    assert(rtc_clock_diagnostics().ppm == -870);
    rtc_clock_tick();
    assert(stored_ppm == -870 && coefficient_saves == 0);
    assert(rtc_clock_prepare_sleep());
    assert(rtc_clock_commit_sleep(true));
    assert(rtc_clock_retained.session.ppm == -870);
}

static void check_history_init_recovery(void) {
    history_test_start();
    reboot_runtime();
    rtc_clock_boot(false, true);
    history_init_fail = true;
    rtc_clock_begin_normal_wake();
    history_test_utc = (uint64_t)wall_us;
    rtc_clock_reference_t ref = reference_now(++history_test_id, history_test_utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_history_owner_summary().store_status == RTC_HISTORY_STORE_IO_ERROR);
    rtc_clock_tick();
    assert(history_init_calls == 2);
    history_test_window();
    const unsigned attempts = history_init_calls;
    history_init_fail = false;
    rtc_clock_tick();
    rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
    assert(history_init_calls == attempts + 1);
    assert(view.ready && view.pending_count == 0 && view.durable_count == 1);
    assert(view.active_ppm == -870 && stored_ppm == -870);
}

static void check_history_populated_init_recovery(void) {
    history_test_start();
    for (unsigned i = 0; i < 4; ++i) history_test_window();
    rtc_clock_tick();
    rtc_clock_history_summary_t before = rtc_clock_history_owner_summary();
    assert(before.accepted_sequence == 4 && before.dropped == 2 && before.durable_sequence == 1);
    reboot_runtime();
    rtc_clock_boot(false, true);
    history_init_fail = true;
    rtc_clock_begin_normal_wake();
    history_test_utc = (uint64_t)wall_us;
    rtc_clock_reference_t ref = reference_now(++history_test_id, history_test_utc);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    history_test_window();
    assert(rtc_clock_history_owner_summary().pending_count == 1);
    history_init_fail = false;
    rtc_clock_tick();
    rtc_clock_history_summary_t after = rtc_clock_history_owner_summary();
    assert(after.ready && after.durable_count == 2 && after.pending_count == 0);
    assert(after.accepted_sequence == 5 && after.durable_sequence == 5 && after.dropped == 2);
    rtc_history_page_t page;
    assert(rtc_clock_history_owner_page(after.generation, 0, &page));
    assert(page.rows[0].sequence == 1 && page.rows[1].sequence == 5);
    assert(page.rows[0].epoch != page.rows[1].epoch);
    assert(after.active_ppm == -870 && !coefficient_saves);
}

static void check_history_shadow_reload_and_age(void) {
    history_test_start();
    for (unsigned i = 0; i < 3; ++i) {
        history_test_window();
        rtc_clock_tick();
    }
    rtc_clock_history_summary_t view = rtc_clock_history_owner_summary();
    assert(view.shadow.result == RTC_HISTORY_ESTIMATE_READY);
    const double candidate = view.shadow.ppm;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    ticks += 1000;
    uptime_us = 3000;
    reboot_runtime();
    assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
    rtc_clock_begin_normal_wake();
    view = rtc_clock_history_owner_summary();
    assert(view.shadow.result == RTC_HISTORY_ESTIMATE_READY);
    assert(fabs(view.shadow.ppm - candidate) < 0.000001);
    advance_fake((int64_t)RTC_HISTORY_EVIDENCE_MAX_AGE_US + 1);
    rtc_clock_tick();
    view = rtc_clock_history_owner_summary();
    assert(view.shadow.result == RTC_HISTORY_ESTIMATE_PENDING && view.shadow.samples == 0);
    assert(view.active_ppm == -870 && stored_ppm == -870);
}

static void simulate_light_sleep(uint64_t duration_us) {
    const uint64_t start = ticks;
    advance_fake((int64_t)duration_us);
    assert(rtc_light_sleep_record(&light_snapshot, start, ticks,
        3U * RTC_DRIFT_Q19, light_period, uptime_us));
}

static void check_light_sleep_coordinates_and_utc(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        reset_all();
        stored_ppm = sign * 10000;
        coefficient_store = RTC_CLOCK_STORE_VALID;
        rtc_clock_boot(false, true);
        assert(rtc_clock_start_light_sleep());
        advance_fake(60000);
        const int64_t initial_wall = wall_us;
        simulate_light_sleep(600000);
        advance_fake(30000);
        assert(rtc_clock_service_light_sleep());
        rtc_clock_diagnostics_t d = rtc_clock_diagnostics();
        assert(d.awake_total_us == 90000 && d.raw_chain_us == 600000);
        const uint64_t corrected = UINT64_C(600000000000) / (uint32_t)(1000000 + stored_ppm);
        assert(d.corrected_chain_us == corrected);
        assert(wall_us == initial_wall + (int64_t)corrected + 30000);
        const unsigned applied = sets;
        assert(rtc_clock_service_light_sleep());
        assert(sets == applied && rtc_clock_diagnostics().awake_total_us == 90000);
        simulate_light_sleep(300000);
        assert(rtc_clock_service_light_sleep());
        d = rtc_clock_diagnostics();
        assert(d.awake_total_us == 90000 && d.raw_chain_us == 900000);
        assert(rtc_clock_prepare_sleep());
        simulate_light_sleep(3000);
        assert(rtc_clock_commit_sleep(true));
        assert(rtc_clock_retained.accounting.awake_total_us == 90000);
        const uint64_t before_raw = rtc_clock_retained.accounting.raw_total.us;
        ticks += 100000;
        wall_us += 300000;
        reboot_runtime();
        assert(rtc_clock_boot(true, true) == RTC_CLOCK_RESTORED);
        assert(rtc_clock_diagnostics().raw_chain_us == before_raw + 300000);
    }
}

static void check_light_sleep_reference_and_failures(void) {
    reset_all();
    rtc_clock_boot(false, true);
    assert(rtc_clock_start_light_sleep());
    rtc_clock_tick();
    rtc_clock_reference_t ref = reference_now(1, (uint64_t)wall_us);
    simulate_light_sleep(300000);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(!rtc_clock_retained.accounting.learning.baseline_valid);
    ref = reference_now(2, (uint64_t)wall_us);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_retained.accounting.learning.baseline_valid);
    assert(rtc_clock_retained.accounting.learning.baseline.awake_us == 0);
    const uint64_t uncertainty = rtc_clock_retained.accounting.learning.baseline.uncertainty_us;
    simulate_light_sleep(300000);
    assert(rtc_clock_service_light_sleep());
    assert(rtc_clock_retained.accounting.learning.baseline.uncertainty_us > uncertainty);

    reset_all();
    rtc_clock_boot(false, true);
    light_start_fail = true;
    assert(!rtc_clock_start_light_sleep());
    light_start_fail = false;
    assert(rtc_clock_start_light_sleep());
    light_snapshot.fault = true;
    assert(!rtc_clock_service_light_sleep() && light_inhibited);

    reset_all();
    stored_ppm = 10000; coefficient_store = RTC_CLOCK_STORE_VALID;
    rtc_clock_boot(false, true);
    assert(rtc_clock_start_light_sleep());
    simulate_light_sleep(300000);
    fail_set = true;
    assert(!rtc_clock_service_light_sleep() && light_inhibited);
    assert(rtc_clock_diagnostics().ppm_qualified);
    fail_set = false;
    assert(!rtc_clock_service_light_sleep()); // No partial retry or duplicate UTC.
}

static void check_light_sleep_fractions_and_manual_sync(void) {
    reset_all();
    rtc_clock_boot(false, true);
    assert(rtc_clock_start_light_sleep());
    light_period = 3U * RTC_DRIFT_Q19 + RTC_DRIFT_Q19 / 2;
    const int64_t before = wall_us;
    for (unsigned i = 0; i < 100; ++i) {
        simulate_light_sleep(3);
        assert(rtc_clock_service_light_sleep());
        assert(wall_us == before + (int64_t)((i + 1) * 7 / 2));
    }
    assert(rtc_clock_diagnostics().awake_total_us == 0);
    assert(rtc_clock_diagnostics().raw_chain_us == 350);
    simulate_light_sleep(300000);
    wall_us = INT64_C(1800000000123456); // The caller has just set external UTC.
    const unsigned old_sets = sets;
    rtc_clock_wall_time_set();
    assert(wall_us == INT64_C(1800000000123456) && sets == old_sets);
    assert(rtc_clock_service_light_sleep());
    assert(wall_us == INT64_C(1800000000123456));
    assert(rtc_clock_diagnostics().raw_chain_us == 350350);
    assert(rtc_clock_diagnostics().awake_total_us == 0);
}

static void check_light_sleep_failed_correction_cannot_restore_trust(void) {
    reset_all();
    stored_ppm = 10000; coefficient_store = RTC_CLOCK_STORE_VALID;
    rtc_clock_boot(false, true);
    assert(rtc_clock_start_light_sleep());
    simulate_light_sleep(300000);
    fail_set = true;
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(!rtc_clock_retained.session.utc_trusted);
    assert(rtc_clock_abort_sleep());
    fail_set = false;
    rtc_clock_reference_t ref = reference_now(42, (uint64_t)wall_us);
    assert(rtc_clock_apply_reference(&ref) == RTC_CLOCK_REFERENCE_SET);
    assert(rtc_clock_prepare_sleep() && rtc_clock_commit_sleep(true));
    assert(rtc_clock_retained.session.utc_trusted);
}

static void check_light_sleep_sdk_libc_offset(void) {
    reset_all();
    rtc_clock_boot(false, true);
    assert(rtc_clock_start_light_sleep());
    const int64_t before = wall_us;
    advance_fake(999999);
    simulate_light_sleep(30000);
    // esp_set_time_from_rtc() overwrites the libc RTC/HRT offset after sleep.
    light_snapshot.wall_offset_total_us = 100;
    wall_us += 100;
    assert(rtc_clock_service_light_sleep());
    assert(wall_us == before + 1029999);
    assert(rtc_clock_diagnostics().awake_total_us == 999999);
    simulate_light_sleep(30000);
    light_snapshot.wall_offset_total_us -= 250;
    wall_us -= 250;
    assert(rtc_clock_service_light_sleep());
    assert(wall_us == before + 1059999);
    const unsigned old_sets = sets;
    assert(rtc_clock_service_light_sleep());
    assert(sets == old_sets);
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "init")) check_history_init_recovery();
        else if (!strcmp(argv[1], "populated")) check_history_populated_init_recovery();
        else if (!strcmp(argv[1], "shadow")) check_history_shadow_reload_and_age();
        else return 2;
        reboot_runtime();
        return 0;
    }
    check_light_sleep_coordinates_and_utc();
    check_light_sleep_reference_and_failures();
    check_light_sleep_fractions_and_manual_sync();
    check_light_sleep_failed_correction_cannot_restore_trust();
    check_light_sleep_sdk_libc_offset();
    check_history_init_recovery();
    check_history_populated_init_recovery();
    check_history_shadow_reload_and_age();
    check_shadow_active_isolation();
    check_history_capture_transaction();
    check_history_corruption();
    check_history_queue_and_cold();
    check_history_persistence_failures();
    check_history_reboot_after_manifest_commit();
    check_history_setter_environment();
    check_history_maintenance_io_isolation();
    check_awake_only_reference_transaction();
    check_corrupt_boolean_is_rejected_before_decode();
    check_unresolved_bridge_cannot_train();
    check_late_reference_does_not_publish_candidate();
    check_maintenance_diagnostics_and_measurement_fault();
    check_source_model_isolation_and_capture_uncertainty();
    check_saved_coefficient_significance();
    check_maintenance_abort_and_awake();
    check_reference_transaction_and_coordinates();
    check_reference_before_aborted_commit();
    check_discontinuities();
    check_untrusted_restore_breaks_learning();
    check_learning_and_coefficient_budget();
    check_saved_coefficient_fractional_chain();
    check_sdk_diagnostic_phase_and_slow_sample();
    check_optional_sdk_diagnostic();
    check_sleep_rate_requires_new_measurement();
    check_read_only_diagnostics();
    check_maintenance_fault_latch();
    check_absolute_bridge_and_consume();
    check_failed_apply_retry_and_cold();
    check_corrupt_state_and_external_sync();
    check_sampling_and_shutdown_failure();
    check_storage_budget_and_failure();
    check_capture_skew_and_retry();
    check_rejected_old_period_is_not_restored();
    puts("RTC adapter UTC, lifecycle, security isolation and storage budget: PASS");
    return 0;
}
