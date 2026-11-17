/* ESP-IDF 5.5.6 coupling is confined here. The debug RTC stamps bracket
 * exactly the interval sleep_modes.c reconstructs into esp_timer. PM callback
 * duration also includes awake work, so it must never be used as sleep time. */
#include "rtc_light_sleep.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_private/esp_sleep_internal.h"
#include "esp_private/pm_impl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "soc/rtc.h"

#if !CONFIG_ESP_SLEEP_DEBUG || !CONFIG_PM_LIGHT_SLEEP_CALLBACKS || !CONFIG_FREERTOS_USE_TICKLESS_IDLE || !CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT
#error "RTC light sleep accounting requires SDK sleep debug, PM callbacks and RTC/HRT libc time"
#endif

#define DIRECT_MAX_AGE_US INT64_C(120000000)
/* Unique sentinel: PM may execute enter/exit without calling the SDK sleep
 * routine at all. Such an attempt must not replay its preceding debug record. */
#define REQUEST_NOT_RUN ESP_ERR_INVALID_STATE

static DRAM_ATTR portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR rtc_light_sleep_snapshot_t s_total;
static DRAM_ATTR esp_sleep_context_t s_context;
static DRAM_ATTR uint32_t s_period, s_source;
/* Pinned private ESP-IDF 5.5.6 newlib symbol (esp_time_impl.c): successful
 * light sleep also changes wall time's RTC-HRT offset in esp_set_time_from_rtc.
 * Reading the offset directly avoids gettimeofday's libc locks in PM callbacks.
 * ESP32-C3 is single-core; PM's critical section excludes normal-task writers.
 * Re-audit this coupling when upgrading IDF or changing libc clock settings. */
extern int64_t s_microseconds_offset;
static DRAM_ATTR int64_t s_period_stamp, s_wall_offset_at_enter;
static DRAM_ATTR bool s_started, s_inhibited;

static bool IRAM_ATTR should_skip(void)
{
    portENTER_CRITICAL(&s_mux);
    const int64_t now = esp_timer_get_time();
    if (s_started && (uint32_t)rtc_clk_slow_src_get() != s_source) s_total.fault = true;
    const bool skip = !s_started || s_inhibited || s_total.fault ||
        now < s_period_stamp || now - s_period_stamp > DIRECT_MAX_AGE_US;
    portEXIT_CRITICAL(&s_mux);
    return skip;
}

static esp_err_t IRAM_ATTR rtc_light_sleep_enter(int64_t duration_us, void *arg)
{
    (void)duration_us;
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    s_context.sleep_request_result = REQUEST_NOT_RUN;
    s_wall_offset_at_enter = s_microseconds_offset;
    s_context.sleep_in_rtc_time_stamp = UINT64_MAX;
    s_context.sleep_out_rtc_time_stamp = UINT64_MAX;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR rtc_light_sleep_exit(int64_t duration_us, void *arg)
{
    (void)duration_us;
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    const esp_err_t result = s_context.sleep_request_result;
    s_context.sleep_request_result = REQUEST_NOT_RUN;
    if (result != REQUEST_NOT_RUN) {
        if ((uint32_t)rtc_clk_slow_src_get() != s_source) {
            s_total.fault = true;
        } else if (result != ESP_OK) {
            if (s_total.reject_count == UINT32_MAX) s_total.fault = true;
            else ++s_total.reject_count;
        } else {
            rtc_light_sleep_snapshot_t next = s_total;
            int64_t delta, wall_total;
            if (__builtin_sub_overflow(s_microseconds_offset, s_wall_offset_at_enter, &delta) ||
                __builtin_add_overflow(s_total.wall_offset_total_us, delta, &wall_total) ||
                !rtc_light_sleep_record(&next,
                    s_context.sleep_in_rtc_time_stamp, s_context.sleep_out_rtc_time_stamp,
                    esp_clk_slowclk_cal_get(), s_period, esp_timer_get_time())) {
                s_total.fault = true;
            } else {
                next.wall_offset_total_us = wall_total;
                s_total = next;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

static esp_pm_sleep_cbs_register_config_t s_callbacks = {
    .enter_cb = rtc_light_sleep_enter,
    .exit_cb = rtc_light_sleep_exit,
    .enter_cb_prior = 0,
    .exit_cb_prior = 0,
};

bool rtc_light_sleep_platform_start(uint32_t period_q19, uint32_t source)
{
    portENTER_CRITICAL(&s_mux);
    const bool valid = !s_started && !s_inhibited && rtc_drift_period_valid(period_q19) &&
        source == (uint32_t)rtc_clk_slow_src_get();
    if (valid) {
        s_period = period_q19;
        s_source = source;
        s_period_stamp = esp_timer_get_time();
        s_context.sleep_request_result = REQUEST_NOT_RUN;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!valid) return false;

    /* Caller keeps automatic sleep disabled until this transaction completes. */
    if (esp_pm_register_skip_light_sleep_callback(should_skip) != ESP_OK) return false;
    esp_sleep_set_sleep_context(&s_context);
    if (esp_pm_light_sleep_register_cbs(&s_callbacks) != ESP_OK) {
        /* Registration may fail after installing just the enter callback. */
        (void)esp_pm_light_sleep_unregister_cbs(&s_callbacks);
        esp_sleep_set_sleep_context(NULL);
        (void)esp_pm_unregister_skip_light_sleep_callback(should_skip);
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_started = true;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void rtc_light_sleep_platform_set_period(uint32_t period_q19)
{
    portENTER_CRITICAL(&s_mux);
    if (!rtc_drift_period_valid(period_q19) ||
        (s_started && s_source != (uint32_t)rtc_clk_slow_src_get())) {
        s_total.fault = true;
    } else if (!s_total.fault && !s_inhibited) {
        s_period = period_q19;
        s_period_stamp = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_mux);
}

void rtc_light_sleep_platform_inhibit(void)
{
    portENTER_CRITICAL(&s_mux);
    s_inhibited = true;
    portEXIT_CRITICAL(&s_mux);
}

rtc_light_sleep_snapshot_t rtc_light_sleep_platform_snapshot(void)
{
    portENTER_CRITICAL(&s_mux);
    rtc_light_sleep_snapshot_t snapshot = s_total;
    snapshot.hrt_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    return snapshot;
}
