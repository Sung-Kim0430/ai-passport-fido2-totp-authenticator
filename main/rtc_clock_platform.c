#include "rtc_clock_internal.h"
#include "rtc_history_store.h"
#include "nvs_flash.h"

#include <limits.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "hal/rtc_cntl_ll.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/rtc.h"

/* Keep qualified calibration and write budget over USB/software resets.
 * Every consumer validates version/source/CRC; UTC still requires deep sleep. */
RTC_NOINIT_ATTR rtc_clock_retained_t rtc_clock_retained;
static temperature_sensor_handle_t s_sensor;
static bool s_sensor_enabled;
static const char *TAG = "rtc_clock";

#define CLOCK_MODEL_RECORD_VERSION 1u
#define CLOCK_COEFFICIENT_RECORD_VERSION 1u

typedef struct {
    uint32_t version;
    uint32_t source;
    uint32_t xtal_mhz;
    uint32_t qualified;
    int32_t ppm;
    uint32_t checksum;
} clock_coefficient_record_t;

/* 存储结构不用 bool，先验证原始标志和校验，再构造正常模型。 */
typedef struct {
    uint32_t version;
    uint32_t source;
    uint32_t xtal_mhz;
    uint32_t qualified;
    double coefficients[3];
    double rms_ppm;
    int32_t min_temp_mc;
    int32_t max_temp_mc;
    uint32_t observations;
    uint32_t occupied_bins;
    uint32_t checksum;
} clock_model_record_t;

uint64_t rtc_clock_platform_history_epoch(void)
{
    const uint64_t value = ((uint64_t)esp_random() << 32) | esp_random();
    return value ? value : 1;
}

uint64_t RTC_IRAM_ATTR rtc_clock_platform_ticks(void)
{
    return rtc_cntl_ll_get_rtc_time() & RTC_DRIFT_TICK_MASK;
}

int64_t rtc_clock_platform_uptime_us(void) { return esp_timer_get_time(); }
uint64_t rtc_clock_platform_rtc_us(void) { return esp_rtc_get_time_us(); }

bool rtc_clock_platform_get_wall_us(int64_t *wall_us)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0 || now.tv_sec < 0 ||
        now.tv_sec > INT64_MAX / 1000000 || now.tv_usec < 0 || now.tv_usec >= 1000000) return false;
    const int64_t seconds = (int64_t)now.tv_sec * 1000000;
    if (now.tv_usec > INT64_MAX - seconds) return false;
    *wall_us = seconds + now.tv_usec;
    return true;
}

bool rtc_clock_platform_set_wall_us(int64_t wall_us)
{
    if (wall_us < 0) return false;
    const struct timeval now = {
        .tv_sec = (time_t)(wall_us / 1000000),
        .tv_usec = (suseconds_t)(wall_us % 1000000)
    };
    return settimeofday(&now, NULL) == 0;
}

uint32_t rtc_clock_platform_source(void)
{
    return (uint32_t)rtc_clk_slow_src_get();
}

bool rtc_clock_platform_supported(void)
{
    const uint32_t source = rtc_clock_platform_source();
    return (source == SOC_RTC_SLOW_CLK_SRC_RC_SLOW || source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256) &&
           (uint32_t)rtc_clk_xtal_freq_get() == 40;
}

uint32_t rtc_clock_platform_measure(void)
{
    const uint32_t source = rtc_clock_platform_source();
    if (!rtc_clock_platform_supported()) return 0;
    const uint32_t period = rtc_clk_cal(RTC_CAL_RTC_MUX, 3000);
    return rtc_clock_platform_source() == source && rtc_clock_platform_supported() ? period : 0;
}

bool rtc_clock_platform_temperature(int32_t *temp_mc)
{
    if (!s_sensor) {
        const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        const esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Temperature installation failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (!s_sensor_enabled) {
        if (temperature_sensor_enable(s_sensor) != ESP_OK) return false;
        s_sensor_enabled = true;
    }
    float value;
    const bool read_ok = temperature_sensor_get_celsius(s_sensor, &value) == ESP_OK &&
                         isfinite(value) && value >= -10 && value <= 80;
    const esp_err_t disabled = temperature_sensor_disable(s_sensor);
    if (disabled == ESP_OK) s_sensor_enabled = false;
    if (!read_ok || disabled != ESP_OK) return false;
    const float scaled = value * 1000.0f;
    *temp_mc = (int32_t)(scaled + (scaled >= 0 ? 0.5f : -0.5f));
    return true;
}

bool rtc_clock_platform_sensor_stop(void)
{
    if (!s_sensor) return true;
    if (s_sensor_enabled) {
        if (temperature_sensor_disable(s_sensor) != ESP_OK) return false;
        s_sensor_enabled = false;
    }
    if (temperature_sensor_uninstall(s_sensor) != ESP_OK) return false;
    s_sensor = NULL;
    return true;
}

rtc_clock_store_result_t rtc_clock_platform_load(rtc_drift_model_t *model)
{
    const uint32_t source = rtc_clock_platform_source();
    const char *key = source == SOC_RTC_SLOW_CLK_SRC_RC_SLOW ? "model_v1" :
        source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "model_fast_v1" : NULL;
    if (!model || !key || !rtc_clock_platform_supported()) return RTC_CLOCK_STORE_BLOCKED;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rtc_cal", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_CLOCK_STORE_MISSING;
    if (err != ESP_OK) return RTC_CLOCK_STORE_BLOCKED;
    clock_model_record_t record;
    size_t size = sizeof record;
    err = nvs_get_blob(handle, key, &record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_CLOCK_STORE_MISSING;
    if (err != ESP_OK || size != sizeof record || record.version != CLOCK_MODEL_RECORD_VERSION ||
        record.source != source || rtc_clock_platform_source() != source || record.xtal_mhz != 40 ||
        record.qualified != 1 || record.checksum != rtc_clock_checksum(&record, offsetof(clock_model_record_t, checksum))) {
        return RTC_CLOCK_STORE_BLOCKED;
    }
    rtc_drift_model_t candidate = {
        .coeff_q19 = {record.coefficients[0], record.coefficients[1], record.coefficients[2]},
        .rms_ppm = record.rms_ppm, .min_temp_mc = record.min_temp_mc,
        .max_temp_mc = record.max_temp_mc, .observations = record.observations,
        .occupied_bins = record.occupied_bins, .qualified = true
    };
    if (!rtc_drift_model_validate(&candidate)) return RTC_CLOCK_STORE_BLOCKED;
    *model = candidate;
    return RTC_CLOCK_STORE_VALID;
}

bool rtc_clock_platform_save(const rtc_drift_model_t *model)
{
    const uint32_t source = rtc_clock_platform_source();
    const char *key = source == SOC_RTC_SLOW_CLK_SRC_RC_SLOW ? "model_v1" :
        source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "model_fast_v1" : NULL;
    if (!key || !rtc_clock_platform_supported() || !rtc_drift_model_validate(model)) return false;
    clock_model_record_t record = {0};
    record.version = CLOCK_MODEL_RECORD_VERSION;
    record.source = source;
    record.xtal_mhz = 40;
    record.qualified = 1;
    for (unsigned i = 0; i < 3; ++i) record.coefficients[i] = model->coeff_q19[i];
    record.rms_ppm = model->rms_ppm;
    record.min_temp_mc = model->min_temp_mc;
    record.max_temp_mc = model->max_temp_mc;
    record.observations = model->observations;
    record.occupied_bins = model->occupied_bins;
    record.checksum = rtc_clock_checksum(&record, offsetof(clock_model_record_t, checksum));
    nvs_handle_t handle;
    if (nvs_open("rtc_cal", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, &record, sizeof record);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

rtc_clock_store_result_t rtc_clock_platform_load_coefficient(int32_t *ppm)
{
    const uint32_t source = rtc_clock_platform_source();
    const char *key = source == SOC_RTC_SLOW_CLK_SRC_RC_SLOW ? "ppm_slow_v1" :
        source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "ppm_fast_v1" : NULL;
    if (!ppm || !key || !rtc_clock_platform_supported()) return RTC_CLOCK_STORE_BLOCKED;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rtc_cal", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_CLOCK_STORE_MISSING;
    if (err != ESP_OK) return RTC_CLOCK_STORE_BLOCKED;
    clock_coefficient_record_t record;
    size_t size = sizeof record;
    err = nvs_get_blob(handle, key, &record, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_CLOCK_STORE_MISSING;
    if (err != ESP_OK || size != sizeof record || record.version != CLOCK_COEFFICIENT_RECORD_VERSION ||
        record.source != source || rtc_clock_platform_source() != source || !rtc_clock_platform_supported() ||
        record.xtal_mhz != 40 || record.qualified != 1 || record.ppm < -20000 || record.ppm > 20000 ||
        record.checksum != rtc_clock_checksum(&record, offsetof(clock_coefficient_record_t, checksum))) {
        return RTC_CLOCK_STORE_BLOCKED;
    }
    *ppm = record.ppm;
    return RTC_CLOCK_STORE_VALID;
}

bool rtc_clock_platform_save_coefficient(int32_t ppm)
{
    const uint32_t source = rtc_clock_platform_source();
    const char *key = source == SOC_RTC_SLOW_CLK_SRC_RC_SLOW ? "ppm_slow_v1" :
        source == SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256 ? "ppm_fast_v1" : NULL;
    if (ppm < -20000 || ppm > 20000 || !key || !rtc_clock_platform_supported()) return false;
    clock_coefficient_record_t record = {0};
    record.version = CLOCK_COEFFICIENT_RECORD_VERSION;
    record.source = source;
    record.xtal_mhz = 40;
    record.qualified = 1;
    record.ppm = ppm;
    record.checksum = rtc_clock_checksum(&record, offsetof(clock_coefficient_record_t, checksum));
    nvs_handle_t handle;
    if (nvs_open("rtc_cal", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, &record, sizeof record);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static rtc_history_store_result_t history_nvs_read(void *u, const char *key, void *data, size_t *size)
{
    (void)u;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("rtc_hist", "hist", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_HISTORY_STORE_MISSING;
    if (err != ESP_OK) return RTC_HISTORY_STORE_IO_ERROR;
    err = nvs_get_blob(handle, key, data, size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return RTC_HISTORY_STORE_MISSING;
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return RTC_HISTORY_STORE_BLOCKED;
    return err == ESP_OK ? RTC_HISTORY_STORE_VALID : RTC_HISTORY_STORE_IO_ERROR;
}

static bool history_nvs_write(void *u, const char *key, const void *data, size_t size)
{
    (void)u;
    nvs_handle_t handle;
    if (nvs_open_from_partition("rtc_hist", "hist", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool history_nvs_commit(void *u)
{
    (void)u;
    return true;
}

rtc_history_store_result_t rtc_history_store_platform_io(rtc_history_store_io_t *io)
{
    if (!io) return RTC_HISTORY_STORE_BLOCKED;
    memset(io, 0, sizeof *io);
    esp_err_t err = nvs_flash_init_partition("rtc_hist");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        return RTC_HISTORY_STORE_BLOCKED;
    if (err != ESP_OK) return RTC_HISTORY_STORE_IO_ERROR;
    nvs_handle_t handle;
    err = nvs_open_from_partition("rtc_hist", "hist", NVS_READWRITE, &handle);
    if (err != ESP_OK) return RTC_HISTORY_STORE_IO_ERROR;
    nvs_close(handle);
    *io = (rtc_history_store_io_t){NULL, history_nvs_read, history_nvs_write, history_nvs_commit};
    return RTC_HISTORY_STORE_VALID;
}

bool rtc_clock_platform_arm(bool request_maintenance)
{
#if CONFIG_AUTH_RTC_MAINTENANCE
    esp_set_deep_sleep_wake_stub(esp_wake_deep_sleep);
    if (request_maintenance && esp_sleep_enable_timer_wakeup(UINT64_C(900000000)) == ESP_OK) return true;
#elif CONFIG_AUTH_RTC_WAKE_STUB
    if (request_maintenance && rtc_clock_stub_install()) return true;
#else
    (void)request_maintenance;
#endif
    // 配置关闭或故障后不能让旧入口指针继续跨睡眠保留。
    esp_set_deep_sleep_wake_stub(esp_wake_deep_sleep);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    return false;
}

bool rtc_clock_platform_arm_interval(uint64_t interval_us)
{
#if CONFIG_AUTH_RTC_MAINTENANCE
    esp_set_deep_sleep_wake_stub(esp_wake_deep_sleep);
    if (interval_us && esp_sleep_enable_timer_wakeup(interval_us) == ESP_OK) return true;
#else
    (void)interval_us;
#endif
    return rtc_clock_platform_arm(false);
}
