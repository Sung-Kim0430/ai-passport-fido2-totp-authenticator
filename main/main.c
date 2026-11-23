/* 认证器应用入口：按钮与无线回调只投递事件，应用任务串行处理存储和状态转换。
 * refresh_display 在 LVGL 锁外准备验证码，锁内只更新已有控件；慢操作不占绘制锁。
 * erase_credentials 仅由确认操作或既有自动擦除策略调用；失败不提前清除错误次数。
 * finish_automatic_wipe 在实际擦除后保存零计数，防止断电或重启绕过既定阈值。
 * 音频播放前唤醒，启用但空闲时待机，关闭开关时软件关断；不把它当作 VDD 断电。
 * 安全计数使用 NVS，锁定剩余时长使用 RTC；UTC 校时不改变锁定倒计时。
 * auth_tick_task 只做传输保活和有界采样，不能在其中读取并修改应用页面状态。
 */
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "key_guard.h"

#include "auth_storage.h"
#include "rtc_clock.h"
#include "rtc_light_sleep.h"
#include "rtc_trace.h"
#include "totp_crypto.h"
#include "passcode_guard.h"
#include "fido_protocol.h"
#include "auth_ble.h"
#include "auth_wifi.h"
#include "auth_ui.h"
#include "fido_pin.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "nvs_flash.h"
#include "esp_pm.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "hal/rtc_cntl_ll.h"
#include "esp_intr_alloc.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "soc/soc.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "main";

typedef enum {
    EVT_KEY = 0,
    EVT_FIDO_REQ,
    EVT_TICK,
    EVT_SLEEP,
    EVT_KEY_IDLE,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            bsp_btn_t btn;
            bsp_btn_ev_t ev;
            uint64_t when_ms;
        } key;
        fido_parsed_cmd_t fido_cmd;
    } data;
} app_event_t;

// 按键初始化前武装，此后守卫仅由共享定时器中的 on_key 更新。
static key_guard_t s_key_guard;
static volatile bool s_input_ready = false;

static QueueHandle_t s_evt_queue;
static auth_config_t s_cfg;
static auth_lab_config_t s_lab_cfg;
static auth_totp_view_t s_totp_view = AUTH_TOTP_VIEW_CARD;
static passcode_guard_t s_guard;
static fido_session_t s_fido;
static uint8_t s_cur_acc_idx = 0;
static uint8_t s_settings_idx = 0;
static uint8_t s_change_pin_step = 0;
static uint8_t s_lab_idx = 0;
static uint8_t s_wifi_idx = 0;
static uint8_t s_clock_mode_idx;
static uint8_t s_clock_mode_page;
static bool s_clock_mode_confirm;
static bool s_clock_mode_save_failed;
static bool s_quartz_blank;
static bool s_quartz_wake_guard;
static void on_ble_cmd(const fido_parsed_cmd_t *cmd);
static bool quartz_standby_enter(void);
static void quartz_standby_wake(void);
static uint8_t s_fido_acc_idx = 0;
static bool s_fido_acc_confirm = false;
static auth_fido_site_t s_fido_rps[AUTH_MAX_FIDO_RPS];
static uint8_t s_fido_rp_count = 0;
static bool s_wipe_confirm = false;
static bool s_fido_reset_confirm = false;

static uint8_t s_set_time_field = 0; /* 0=Year, 1=Month, 2=Day, 3=Hour, 4=Minute */
static int s_set_year = 2026;
static int s_set_mon  = 1;
static int s_set_day  = 1;
static int s_set_hour = 0;
static int s_set_min  = 0;
static int s_set_sec  = 0;

static char s_lock_msg_buf[64] = "Press [OK] to unlock";
static char s_cp_msg_buf[64]   = "[OK] Next | Long [OK] Cancel";
static bool s_cp_is_error      = false;

static const int8_t s_tz_options[] = { 8, 9, 10, 12, -8, -7, -6, -5, -4, 0, 1, 2, 3, 5, 7 };
#define TZ_OPTIONS_COUNT (sizeof(s_tz_options) / sizeof(s_tz_options[0]))

static volatile bool s_session_unlocked = false;
static auth_view_t s_view_before_modal = AUTH_VIEW_LOCK;
static volatile uint32_t s_last_activity_ms = 0;
static uint64_t s_ignore_key_before_ms;

#ifdef ESP_PLATFORM
static RTC_DATA_ATTR uint8_t s_rtc_failed_attempts = 0;
static RTC_DATA_ATTR int64_t s_rtc_lockout_end_us = 0;
#endif

static const uint16_t s_sleep_timeout_options[] = { 15, 30, 60, 120, 300, 0 };
#define SLEEP_TIMEOUT_OPTIONS_COUNT (sizeof(s_sleep_timeout_options) / sizeof(s_sleep_timeout_options[0]))

static int days_in_month(int year, int mon)
{
    static const int dpm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon == 2) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29 : 28;
    }
    if (mon >= 1 && mon <= 12) return dpm[mon - 1];
    return 31;
}

static int64_t calculate_utc_epoch(int year, int mon, int day, int hour, int min, int sec, int8_t tz_offset)
{
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    static const int dpm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < mon; m++) {
        if (m == 2) {
            bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
            days += leap ? 29 : 28;
        } else {
            days += dpm[m - 1];
        }
    }
    days += (day - 1);
    int64_t local_sec = days * 86400LL + hour * 3600LL + min * 60LL + sec;
    int64_t utc_sec = local_sec - (int64_t)tz_offset * 3600LL;
    return utc_sec > 0 ? utc_sec : 0;
}

static volatile bool s_clock_key_seen;
static bool s_clock_observer_owned;

static void IRAM_ATTR clock_key_isr(void *argument)
{
    (void)argument;
    s_clock_key_seen = true;
}

#if !CONFIG_AUTH_RTC_WAKE_STUB
/* Runs immediately from RTC Fast Memory on deep-sleep wake before ROM loads the
 * bootloader so GPIO21 stays clamped low and held across ROM/bootloader entry. */
void RTC_IRAM_ATTR esp_wake_deep_sleep(void)
{
    const uint64_t wake_tick = rtc_cntl_ll_get_rtc_time();
    rtc_trace_store.wake_tick = wake_tick;
    rtc_trace_store.wake_tick_inverse = ~wake_tick;
    esp_default_wake_deep_sleep();
#if CONFIG_IDF_TARGET_ESP32C3
    gpio_ll_pullup_dis(&GPIO, BSP_LCD_BL);
    gpio_ll_pulldown_dis(&GPIO, BSP_LCD_BL);
    gpio_ll_input_disable(&GPIO, BSP_LCD_BL);
    gpio_ll_od_disable(&GPIO, BSP_LCD_BL);
    gpio_ll_matrix_out_default(&GPIO, BSP_LCD_BL);
    gpio_ll_set_level(&GPIO, BSP_LCD_BL, 0);
    gpio_ll_func_sel(&GPIO, BSP_LCD_BL, PIN_FUNC_GPIO);
    gpio_ll_output_enable(&GPIO, BSP_LCD_BL);
    SET_PERI_REG_MASK(RTC_CNTL_DIG_PAD_HOLD_REG, (UINT32_C(1) << BSP_LCD_BL));
#endif
}
#endif

static bool clock_key_pending(void)
{
    if (gpio_get_level(BSP_BTN_GPIO) == 0 || (REG_READ(GPIO_STATUS_REG) & (1UL << BSP_BTN_GPIO)))
        s_clock_key_seen = true;
    return s_clock_key_seen;
}

static void clock_key_observer_end(void)
{
    if (!s_clock_observer_owned) return;
    gpio_intr_disable(BSP_BTN_GPIO);
    gpio_isr_handler_remove(BSP_BTN_GPIO);
    gpio_uninstall_isr_service();
    s_clock_observer_owned = false;
}

static bool clock_key_observer_begin(void)
{
    s_clock_key_seen = (REG_READ(GPIO_STATUS_REG) & (1UL << BSP_BTN_GPIO)) != 0;
    if (gpio_hold_dis(BSP_BTN_GPIO) != ESP_OK) return false;
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << BSP_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&input) != ESP_OK || clock_key_pending()) return false;
    if (gpio_install_isr_service(ESP_INTR_FLAG_IRAM) != ESP_OK) return false;
    s_clock_observer_owned = true;
    if (gpio_isr_handler_add(BSP_BTN_GPIO, clock_key_isr, NULL) != ESP_OK ||
        gpio_set_intr_type(BSP_BTN_GPIO, GPIO_INTR_NEGEDGE) != ESP_OK ||
        gpio_intr_enable(BSP_BTN_GPIO) != ESP_OK) {
        clock_key_observer_end();
        return false;
    }
    return !clock_key_pending();
}

static void clock_maintenance_try(bool timer_only, bool observer_ready)
{
    if (!timer_only) return;
    rtc_clock_note_maintenance(RTC_CLOCK_MAINTENANCE_ATTEMPT);
    if (clock_key_pending()) {
        rtc_clock_note_maintenance(RTC_CLOCK_MAINTENANCE_KEY);
        return;
    }
    if (!observer_ready) {
        rtc_clock_note_maintenance(RTC_CLOCK_MAINTENANCE_OBSERVER);
        return;
    }
    if (!rtc_clock_maintenance_due(true)) {
        rtc_clock_note_maintenance(RTC_CLOCK_MAINTENANCE_NOT_DUE);
        return;
    }
    rtc_clock_maintenance_reason_t reason = RTC_CLOCK_MAINTENANCE_PREPARE;
    if (!rtc_clock_prepare_maintenance()) goto canceled;
    if (clock_key_pending()) { reason = RTC_CLOCK_MAINTENANCE_KEY; goto canceled; }
    reason = RTC_CLOCK_MAINTENANCE_GPIO;
    if (esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK)
        goto canceled;
    reason = RTC_CLOCK_MAINTENANCE_COMMIT;
    if (!rtc_clock_commit_sleep(auth_time_is_set())) goto canceled;
    if (clock_key_pending()) { reason = RTC_CLOCK_MAINTENANCE_KEY; goto canceled; }
    reason = RTC_CLOCK_MAINTENANCE_REJECT;
    (void)esp_deep_sleep_try_to_start();

canceled:
    (void)rtc_clock_abort_sleep();
    rtc_clock_note_maintenance(reason);
}

/* A failed unattended maintenance must not initialize the screen/radios.
 * Keep a real key observable, stop timer retries, and explicitly revoke UTC
 * trust rather than continuing to display a possibly incorrect time. */
static void clock_maintenance_quiet_fallback(bool timer_only, bool observer_ready)
{
    if (!timer_only || !observer_ready || clock_key_pending()) return;
    (void)rtc_clock_abort_sleep();
    auth_time_init(false);
    if (esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK)
        return;
    if (clock_key_pending()) return;
    esp_deep_sleep_start();
}

static void system_enter_deep_sleep(void)
{
    if (!rtc_mode_deep_sleep(rtc_clock_get_mode())) {
        (void)quartz_standby_enter();
        return;
    }
    ESP_LOGI(TAG, "Entering terminal deep sleep...");

    /* 1. Never cut the radios while the wake button is still held, or the
     * release that got us here is lost and the device can re-enter sleep
     * immediately after waking. */
    if (bsp_button_read_mv() < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "Button currently pressed, delaying sleep");
        return;
    }

    // 模型检查点和传感器卸载仍在可返回阶段，不持有 LVGL 锁。
    if (!rtc_clock_prepare_sleep()) {
        ESP_LOGW(TAG, "Clock sampler shutdown incomplete; delaying sleep");
        return;
    }

    /* 2. Stop Wi-Fi AP & BLE & Turn off backlight */
    if (!auth_wifi_ap_stop() || !auth_ble_stop()) {
        ESP_LOGE(TAG, "Radio shutdown incomplete; refusing deep sleep");
        return;
    }
    bsp_display_backlight(0);

    /* 3. Button release to GPIO digital input + pullup */
    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_button_prepare_deep_sleep failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    if (level != 1) {
        ESP_LOGW(TAG, "Button held during deep sleep prep, restarting");
        esp_restart();
    }

    /* 4. Arm GPIO wakeup on button pin (GPIO0) */
    esp_err_t wake_err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable GPIO wakeup: %s", esp_err_to_name(wake_err));
        esp_restart();
    }

    /* 5. Peripheral suspend in strict required order */
    bsp_battery_sleep();
    bsp_audio_sleep();
    bsp_audio_prepare_deep_sleep();
    bsp_i2c_prepare_deep_sleep();

    /* 6. Display suspend with LVGL lock */
    if (bsp_lvgl_lock(1000)) {
        bsp_display_prepare_deep_sleep();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for display sleep");
        esp_restart();
    }

    // 所有外设交还后才提交 UTC/tick 锚点；之后只允许进入深睡或重启。
    if (!rtc_clock_commit_sleep(auth_time_is_set())) {
        ESP_LOGE(TAG, "Unable to commit RTC continuation");
        esp_restart();
    }

    /* 7. Enter deep sleep */
    esp_deep_sleep_start();
    esp_restart();
}

static inline void ui_lock(void)
{
    bsp_lvgl_lock(-1);
}

static inline void ui_unlock(void)
{
    bsp_lvgl_unlock();
}

static void auth_enter_view(auth_view_t view)
{
    if (view != AUTH_VIEW_LOCK && view != AUTH_VIEW_FIDO_PROMPT && !s_session_unlocked) view = AUTH_VIEW_LOCK;
    if (view != AUTH_VIEW_SETTINGS) s_wipe_confirm = false;
    if (view != AUTH_VIEW_LAB) s_fido_reset_confirm = false;
    if (view != AUTH_VIEW_FIDO_ACCOUNTS) s_fido_acc_confirm = false;
    if (auth_ui_get_view() == AUTH_VIEW_CHANGE_PASSCODE &&
        view != AUTH_VIEW_CHANGE_PASSCODE &&
        view != AUTH_VIEW_FIDO_PROMPT) {
        passcode_guard_init(&s_guard, s_cfg.passcode_pattern, s_cfg.passcode_len);
    }
    ui_lock();
    auth_ui_set_view(view);
    ui_unlock();
}

// 提示音电源由应用任务串行管理：保留偏置的待机与完整软件 Off 分开处理。
static uint64_t s_audio_idle_at_ms;

static void audio_off_now(void)
{
    s_audio_idle_at_ms = 0;
    esp_err_t err = bsp_audio_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "音频 Off 未完成: %s", esp_err_to_name(err));
}

static void audio_standby_now(void)
{
    s_audio_idle_at_ms = 0;
    if (bsp_audio_standby() != ESP_OK) {
        // 待机失败时撤销传输能力，不能让后续播放访问半关闭的接口。
        audio_off_now();
    }
}

static void audio_idle(uint64_t now_ms)
{
    if (s_audio_idle_at_ms && (!s_cfg.sound_enabled || now_ms >= s_audio_idle_at_ms)) {
        if (s_cfg.sound_enabled) audio_standby_now();
        else audio_off_now();
    }
}

static void play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_cfg.sound_enabled || freq_hz == 0 || duration_ms == 0) return;
    if (bsp_audio_wake() != ESP_OK) return;

    const uint32_t sample_rate = 16000;
    uint32_t total_samples = (sample_rate * duration_ms) / 1000;
    if (total_samples == 0) return;

    int16_t buffer[128];
    uint32_t samples_written = 0;
    float phase = 0.0f;
    float phase_inc = (2.0f * 3.14159265f * (float)freq_hz) / (float)sample_rate;

    while (samples_written < total_samples) {
        uint32_t chunk = total_samples - samples_written;
        if (chunk > 128) chunk = 128;

        for (uint32_t i = 0; i < chunk; i++) {
            buffer[i] = (int16_t)(sinf(phase) * 6000.0f);
            phase += phase_inc;
            if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        if (bsp_audio_write((const uint8_t *)buffer, chunk * sizeof(int16_t)) != ESP_OK) {
            audio_off_now();
            return;
        }
        samples_written += chunk;
    }
    s_audio_idle_at_ms = (uint64_t)esp_timer_get_time() / 1000U + 300U;
}

static void sound_click(void)
{
    play_tone(1800, 20);
}

static void sound_success(void)
{
    play_tone(1200, 60);
    vTaskDelay(pdMS_TO_TICKS(30));
    play_tone(1800, 100);
}

static void sound_fail(void)
{
    play_tone(400, 120);
}

static void sound_fido_alert(void)
{
    play_tone(1500, 80);
    vTaskDelay(pdMS_TO_TICKS(40));
    play_tone(2200, 120);
}

static void sound_hint(void)
{
    play_tone(1500, 60);
    vTaskDelay(pdMS_TO_TICKS(40));
    play_tone(2000, 80);
}

static bool clock_mode_apply(rtc_mode_t mode)
{
    if (!rtc_mode_valid(mode) || mode == rtc_clock_get_mode()) return false;
    s_clock_mode_save_failed = !auth_storage_clock_mode_save(mode);
    if (s_clock_mode_save_failed) return false;
    auth_time_init(false);
    esp_restart();
    return true;
}

static bool quartz_standby_enter(void)
{
    if (s_quartz_blank) return true;
    if (rtc_clock_get_mode() != RTC_MODE_XTAL ||
        bsp_button_read_mv() < BSP_BTN_MV_RELEASED_MIN) return false;
    if (!auth_wifi_ap_stop() || !auth_ble_stop()) return false;
    audio_off_now();
    if (!bsp_lvgl_lock(1000)) return false;
    bsp_display_backlight(0);
    const esp_err_t err = esp_lcd_panel_disp_on_off(bsp_display_panel(), false);
    if (err != ESP_OK) bsp_display_backlight(s_cfg.brightness);
    bsp_lvgl_unlock();
    if (err != ESP_OK) return false;
    s_session_unlocked = false;
    passcode_guard_lock(&s_guard);
    auth_enter_view(AUTH_VIEW_LOCK);
    s_quartz_wake_guard = false;
    s_quartz_blank = true;
    return true;
}

static void quartz_standby_wake(void)
{
    if (!s_quartz_blank || !bsp_lvgl_lock(1000)) return;
    const esp_err_t err = esp_lcd_panel_disp_on_off(bsp_display_panel(), true);
    if (err == ESP_OK) bsp_display_backlight(s_cfg.brightness);
    bsp_lvgl_unlock();
    if (err != ESP_OK) return;
    s_quartz_blank = false;
    s_quartz_wake_guard = true;
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_lab_cfg.ble_fido_enabled) {
        if (auth_ble_init(on_ble_cmd)) auth_ble_start_advertising();
        else ESP_LOGW(TAG, "BLE restart after quartz standby failed");
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_IDLE)) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "Button gestures idle, boot guard disarmed");
        }
        if (ev == BSP_BTN_IDLE && !was_armed && s_input_ready && s_evt_queue) {
            const app_event_t idle = {.type = EVT_KEY_IDLE};
            (void)xQueueSend(s_evt_queue, &idle, 0);
        }
        return;
    }
    if (!s_input_ready || !s_evt_queue) return;
    app_event_t evt = {.type = EVT_KEY};
    evt.data.key.btn = btn;
    evt.data.key.ev = ev;
    evt.data.key.when_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    (void)xQueueSend(s_evt_queue, &evt, 0);
}

static void on_ble_cmd(const fido_parsed_cmd_t *cmd)
{
    if (!s_evt_queue || !cmd) return;
    app_event_t evt;
    evt.type = EVT_FIDO_REQ;
    evt.data.fido_cmd = *cmd;
    xQueueSend(s_evt_queue, &evt, 0);
}

static bool on_wifi_cmd(const fido_parsed_cmd_t *cmd)
{
    if (!s_evt_queue || !cmd || !s_session_unlocked) return false;
    app_event_t evt = {.type = EVT_FIDO_REQ};
    evt.data.fido_cmd = *cmd;
    return xQueueSend(s_evt_queue, &evt, 0) == pdTRUE;
}

static uint8_t settings_action_for_index(uint8_t idx)
{
    switch (idx) {
    case 2: return 14;
    case 3: return 11;
    case 4: return 2;
    case 5: return 5;
    case 6: return 3;
    case 7: return 4;
    case 8: return 6;
    case 9: return 7;
    case 10: return 8;
    case 11: return 9;
    case 12: return 10;
    case 13: return 13;
    case 14: return 15;
    case 15: return 12;
    default: return idx;
    }
}

static bool toggle_wifi_hotspot(void)
{
    if (auth_wifi_ap_is_active()) {
        if (!auth_wifi_ap_stop()) {
            ESP_LOGE(TAG, "Wi-Fi shutdown incomplete; BLE remains stopped");
            return false;
        }
        if (s_lab_cfg.ble_fido_enabled) {
            if (!auth_ble_init(on_ble_cmd)) {
                ESP_LOGE(TAG, "Failed to restore BLE after Wi-Fi stop");
                return false;
            }
            auth_ble_start_advertising();
        }
        return true;
    }
    if (!auth_ble_stop()) {
        ESP_LOGE(TAG, "BLE did not stop; refusing to start Wi-Fi");
        return false;
    }
    if (auth_wifi_ap_start(&s_cfg, on_wifi_cmd)) return true;
    ESP_LOGE(TAG, "Wi-Fi AP start failed; restoring BLE");
    if (!auth_wifi_ap_is_active() && s_lab_cfg.ble_fido_enabled && auth_ble_init(on_ble_cmd)) {
        auth_ble_start_advertising();
    }
    return false;
}

static bool erase_credentials(bool include_totp)
{
    bool resume_ble = s_lab_cfg.ble_fido_enabled && !auth_wifi_ap_is_active();
    if (!auth_ble_stop()) return false;
    bool ok = (!include_totp || auth_storage_wipe_all(&s_cfg)) && auth_storage_fido_reset_all();
    if (!ok) {
        ESP_LOGE(TAG, "Credential erase incomplete; BLE remains stopped");
        return false;
    }
    fido_pin_factory_reset();
    auth_ble_reload_seed();
    fido_session_init(&s_fido);
    if (resume_ble) {
        if (!auth_ble_init(on_ble_cmd)) return false;
        auth_ble_start_advertising();
    }
    return true;
}

static bool finish_automatic_wipe(void)
{
    if (!erase_credentials(true)) return false;
    if (!auth_storage_pin_failures_save(0)) return false;
    passcode_guard_init(&s_guard, s_cfg.passcode_pattern, s_cfg.passcode_len);
    s_rtc_failed_attempts = 0;
    s_rtc_lockout_end_us = 0;
    s_cur_acc_idx = 0;
    return true;
}

static uint8_t s_settings_before[offsetof(auth_config_t, account_count)];
static bool s_settings_write_failed;

static bool save_settings(void)
{
    if (auth_storage_save(&s_cfg)) return true;
    memcpy(&s_cfg, s_settings_before, sizeof(s_settings_before));
    bsp_display_backlight(s_cfg.brightness);
    s_settings_write_failed = true;
    ESP_LOGE(TAG, "Settings save failed; previous settings restored");
    return false;
}

/* 应用任务所有：只限频电量 I²C，不降低倒计时刷新；USB 边沿立即重新查询。 */
static int8_t battery_soc_for_frame(uint64_t now_ms, bool usb_connected)
{
    static uint64_t next_poll_ms;
    static int8_t cached_soc = -1;
    static bool previous_usb;
    if (now_ms >= next_poll_ms || usb_connected != previous_usb) {
        cached_soc = bsp_battery_soc();
        next_poll_ms = now_ms + 5000;
        previous_usb = usb_connected;
    }
    return cached_soc;
}

static void battery_environment_for_frame(uint64_t now_ms)
{
    static uint64_t next_poll_ms;
    if (now_ms < next_poll_ms) return;
    next_poll_ms = now_ms + 5000;
    const int millivolts = bsp_battery_mv();
    const bool battery_valid = millivolts >= 2000 && millivolts <= 6000;
    rtc_clock_battery_cache_update(battery_valid, battery_valid ? (uint16_t)millivolts : 0,
        esp_timer_get_time());
}

static void refresh_display(void)
{
    if (s_quartz_blank) return;
    auth_view_t view = auth_ui_get_view();
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uint64_t current_unix = auth_get_current_timestamp(&s_cfg);
    bool ble_conn = auth_ble_is_connected();
    bool is_charging = usb_serial_jtag_is_connected();
    int8_t bat_soc = battery_soc_for_frame(now_ms, is_charging);
    battery_environment_for_frame(now_ms);

    auth_wifi_publish_config(&s_cfg, s_session_unlocked);
    static bool s_batt_80_alerted = false;
    if (s_cfg.battery_protect_80 && is_charging && bat_soc >= 80) {
        if (!s_batt_80_alerted) {
            s_batt_80_alerted = true;
            sound_hint();
        }
    } else if (!s_cfg.battery_protect_80 || !is_charging || bat_soc < 78) {
        s_batt_80_alerted = false;
    }
    auth_totp_list_item_t otp_items[AUTH_TOTP_LIST_MAX];
    char otp_codes[AUTH_TOTP_LIST_MAX][TOTP_MAX_CODE_LEN];
    uint8_t first = 0;
    uint8_t visible = 0;
    if (view == AUTH_VIEW_TOTP && s_cfg.account_count) {
        if (s_cur_acc_idx >= s_cfg.account_count) s_cur_acc_idx = 0;
        first = auth_ui_totp_page_start(s_cur_acc_idx, s_totp_view);
        visible = auth_ui_totp_page_count(s_cfg.account_count, s_cur_acc_idx, s_totp_view);
        for (uint8_t i = 0; i < visible; i++) {
            const auth_totp_account_t *account = &s_cfg.accounts[first + i];
            otp_items[i].account = account;
            otp_items[i].code = NULL;
            otp_items[i].remaining = 0;
            if (auth_time_is_set()) {
                totp_generate_from_base32_ex(account->secret, current_unix, account->period, account->digits,
                                             (totp_algorithm_t)account->algorithm, otp_codes[i], &otp_items[i].remaining);
                otp_items[i].code = otp_codes[i];
            }
        }
    }
    ui_lock();
    auth_ui_update_status_bar(ble_conn, bat_soc, is_charging, s_cfg.battery_protect_80);
    auth_ui_update_top_clock(current_unix, s_cfg.timezone_offset, s_cfg.time_format_24h);
    auth_ui_update_ble_status(auth_ble_is_enabled(), ble_conn);
    auth_ui_update_wifi_status(auth_wifi_ap_is_active());

    if (view == AUTH_VIEW_LOCK) {
        char disp_str[128];
        passcode_guard_get_display_str(&s_guard, disp_str, sizeof(disp_str), false);
        uint32_t rem_lockout = 0;
        bool is_lockout = passcode_guard_is_locked_out(&s_guard, now_ms, &rem_lockout);
        auth_ui_update_lock_screen(disp_str, s_guard.current_len,
                                   s_lock_msg_buf,
                                   is_lockout, rem_lockout,
                                   (int64_t)current_unix, s_cfg.timezone_offset, s_cfg.time_format_24h);
    } else if (view == AUTH_VIEW_TOTP) {
        if (!visible) {
            auth_ui_update_totp(NULL, 0, 0, NULL, 0, 30, ble_conn, bat_soc);
        } else if (s_totp_view == AUTH_TOTP_VIEW_CARD) {
            auth_ui_update_totp(otp_items[0].account, s_cur_acc_idx, s_cfg.account_count,
                                otp_items[0].code, otp_items[0].remaining,
                                otp_items[0].account->period, ble_conn, bat_soc);
        } else {
            auth_ui_update_totp_list(otp_items, first, visible, s_cfg.account_count, s_totp_view);
        }
    } else if (view == AUTH_VIEW_FIDO_PROMPT) {
        uint32_t rem_fido = 0;
        bool timed_out = fido_session_tick(&s_fido, now_ms, &rem_fido);
        if (timed_out) {
            ui_unlock();
            auth_ble_fido_user_timeout();
            auth_ble_send_response("UP_TIMEOUT");
            ui_lock();
            auth_ui_update_fido_result(false);
            ui_unlock();
            sound_fail();
            vTaskDelay(pdMS_TO_TICKS(800));
            s_ignore_key_before_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            auth_enter_view(s_view_before_modal);
            return;
        } else {
            auth_ui_update_fido_prompt(&s_fido.current_req, rem_fido);
        }
    } else if (view == AUTH_VIEW_SETTINGS) {
        auth_ui_update_settings(s_settings_idx, s_cfg.brightness,
                                s_cfg.sound_enabled, s_totp_view, s_cfg.auto_sleep_sec,
                                s_cfg.auto_wipe_enabled, s_cfg.passcode_len,
                                auth_wifi_ap_is_active(),
                                current_unix, s_cfg.timezone_offset,
                                s_cfg.time_format_24h,
                                s_cfg.battery_protect_80,
                                s_wipe_confirm);
    } else if (view == AUTH_VIEW_CHANGE_PASSCODE) {
        char disp_str[128];
        passcode_guard_get_display_str(&s_guard, disp_str, sizeof(disp_str), false);
        auth_ui_update_change_passcode(s_change_pin_step, disp_str, s_guard.current_len,
                                       s_cp_msg_buf, s_cp_is_error);
    } else if (view == AUTH_VIEW_SET_TIME) {
        int max_day = days_in_month(s_set_year, s_set_mon);
        if (s_set_day > max_day) s_set_day = max_day;
        auth_ui_update_set_time(s_set_time_field, s_set_year, s_set_mon, s_set_day,
                                s_set_hour, s_set_min, s_set_sec);
    } else if (view == AUTH_VIEW_LAB) {
        auth_ui_update_lab(s_lab_idx, s_lab_cfg.ble_fido_enabled,
                           s_lab_cfg.console_log_enabled, s_fido_reset_confirm);
    } else if (view == AUTH_VIEW_CLOCK_MODES) {
        auth_ui_update_clock_modes(rtc_clock_get_mode(), s_clock_mode_idx,
                                   s_clock_mode_page, s_clock_mode_confirm,
                                   s_clock_mode_save_failed);
    } else if (view == AUTH_VIEW_WIFI) {
        const auth_identity_t *identity = auth_wifi_get_identity();
        auth_ui_update_wifi(s_wifi_idx, auth_wifi_ap_is_active(),
                            identity ? identity->ssid : "Unavailable",
                            identity ? identity->password : "");
    } else if (view == AUTH_VIEW_FIDO_ACCOUNTS) {
        auth_ui_update_fido_accounts(s_fido_rps, s_fido_rp_count,
                                     s_fido_acc_idx, s_fido_acc_confirm);
    }

    ui_unlock();
}

static void sample_clock_pair(int64_t *hrt, uint64_t *rtc, uint64_t *window)
{
    int64_t before = esp_timer_get_time();
    *rtc = esp_rtc_get_time_us();
    int64_t after = esp_timer_get_time();
    *hrt = before + (after - before) / 2;
    *window = (uint64_t)(after - before);
}

static void auth_tick_task(void *param)
{
    (void)param;
    uint32_t tick_count = 0;
    uint32_t trace_ticks = 0;
    bool clock_trace = usb_serial_jtag_is_connected();
    int64_t hrt_start = 0;
    uint64_t rtc_start = 0, start_window = 0;
    if (clock_trace) sample_clock_pair(&hrt_start, &rtc_start, &start_window);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        auth_ble_tick();
        if (++tick_count >= 5 && s_evt_queue) {
            tick_count = 0;
            if (clock_trace && ++trace_ticks % 10 == 0) {
                int64_t hrt;
                uint64_t rtc, window;
                sample_clock_pair(&hrt, &rtc, &window);
                ESP_LOGI(TAG, "CLOCK_COMPARE hrt_us=%lld rtc_us=%llu diff_us=%lld uncertainty_us=%llu",
                         (long long)(hrt - hrt_start), (unsigned long long)(rtc - rtc_start),
                         (long long)((int64_t)(rtc - rtc_start) - (hrt - hrt_start)),
                         (unsigned long long)((window + start_window + 1) / 2 + 16));
                ESP_LOGI(TAG, "MEMORY free=%u largest=%u min=%u tick_stack_min=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                const rtc_light_sleep_snapshot_t light = rtc_light_sleep_platform_snapshot();
                ESP_LOGI(TAG, "LIGHT_SLEEP count=%lu rejected=%lu sdk_us=%llu fault=%u",
                         (unsigned long)light.sleep_count, (unsigned long)light.reject_count,
                         (unsigned long long)light.sdk_total_us, (unsigned)light.fault);
                if (trace_ticks >= 60) clock_trace = false;
            }
            app_event_t evt = {.type = EVT_TICK};
            xQueueSend(s_evt_queue, &evt, 0);
        }
    }
}

static void auth_app_task(void *param)
{
    (void)param;
    app_event_t evt;
    bool light_fault_reported = false;
    if (s_cfg.auto_wipe_enabled && s_guard.failed_attempts > PASSCODE_WIPE_ATTEMPTS) {
        bool wiped = finish_automatic_wipe();
        snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "%s", wiped ? "TOKENS WIPED" : "WIPE INCOMPLETE");
    }

    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY)) {
        if (!rtc_clock_service_light_sleep() && !light_fault_reported) {
            auth_time_init(false);
            light_fault_reported = true;
            ESP_LOGE(TAG, "Light-sleep clock reconciliation failed; time synchronization required");
        }
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        memcpy(s_settings_before, &s_cfg, sizeof(s_settings_before));
        s_settings_write_failed = false;

        if (evt.type == EVT_KEY_IDLE) {
            s_quartz_wake_guard = false;
            continue;
        }

        if (evt.type == EVT_TICK) {
            audio_idle(now_ms);
            // 采样和模型写入只在应用工作任务，确认请求期间先不增加延迟。
            if (s_fido.state != FIDO_STATE_UP_PENDING) rtc_clock_tick();
            auth_wifi_history_service(s_session_unlocked);
            if (s_quartz_blank) continue;
            uint32_t now = (uint32_t)now_ms;
            bool active = auth_ui_get_view() == AUTH_VIEW_FIDO_PROMPT ||
                          auth_ble_is_connected() || auth_wifi_ap_is_active();
            if (active) s_last_activity_ms = now;
            if (!active && s_cfg.auto_sleep_sec > 0 &&
                now - s_last_activity_ms >= (uint32_t)s_cfg.auto_sleep_sec * 1000U) {
                system_enter_deep_sleep();
            }
            refresh_display();
            continue;
        }

        if (evt.type == EVT_SLEEP) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (s_cfg.auto_sleep_sec > 0 &&
                (now - s_last_activity_ms) >= ((uint32_t)s_cfg.auto_sleep_sec * 1000U) &&
                auth_ui_get_view() != AUTH_VIEW_FIDO_PROMPT &&
                !auth_ble_is_connected() && !auth_wifi_ap_is_active()) {
                system_enter_deep_sleep();
            } else {
                s_last_activity_ms = now;
            }
            continue;
        }

        if (evt.type == EVT_FIDO_REQ) {
            s_last_activity_ms = (uint32_t)now_ms;
            const fido_parsed_cmd_t *cmd = &evt.data.fido_cmd;
            if (s_quartz_blank) {
                auth_wifi_command_result(cmd->request_id, false);
                continue;
            }
            if (s_cfg.auto_wipe_enabled && s_guard.failed_attempts > PASSCODE_WIPE_ATTEMPTS) {
                auth_wifi_command_result(cmd->request_id, false);
                continue;
            }
            bool is_fido = cmd->type == FIDO_CMD_AUTH_REQ || cmd->type == FIDO_CMD_REG_REQ || cmd->type == FIDO_CMD_CANCEL;
            bool edits_existing = cmd->type == FIDO_CMD_EDIT_TOTP || cmd->type == FIDO_CMD_DEL_TOTP;
            if ((!is_fido && !s_session_unlocked) || (!is_fido && s_fido.state == FIDO_STATE_UP_PENDING) ||
                (edits_existing && cmd->request_id && cmd->account_revision != auth_storage_account_revision())) {
                auth_wifi_command_result(cmd->request_id, false);
                continue;
            }
            bool ok = false;
            if (cmd->type == FIDO_CMD_AUTH_REQ || cmd->type == FIDO_CMD_REG_REQ) {
                fido_request_type_t type = cmd->type == FIDO_CMD_REG_REQ ? FIDO_REQ_REGISTRATION : FIDO_REQ_AUTHENTICATION;
                if (s_fido.state != FIDO_STATE_UP_PENDING) {
                    s_view_before_modal = auth_ui_get_view();
                    ok = fido_session_start_req(&s_fido, type, cmd->param1, cmd->param2, cmd->param3, now_ms, 15000);
                    if (ok) {
                        auth_enter_view(AUTH_VIEW_FIDO_PROMPT);
                        sound_fido_alert();
                    }
                }
            } else if (cmd->type == FIDO_CMD_CANCEL) {
                ok = fido_session_reject(&s_fido);
                if (ok) {
                    auth_ble_fido_user_confirm(false);
                    auth_enter_view(s_view_before_modal);
                }
            } else if (cmd->type == FIDO_CMD_SYNC_TIME) {
                if (cmd->time_reference_present) {
                    const rtc_clock_reference_t reference = {
                        .id = cmd->time_reference_id,
                        .utc_us = cmd->time_reference_utc_us,
                        .uncertainty_us = cmd->time_reference_uncertainty_us,
                        .hrt_us = cmd->time_reference_hrt_us,
                        .quality = cmd->time_reference_quality,
                        .max_age_us = cmd->time_reference_precise ? 5000000 : 0,
                    };
                    ok = auth_set_reference_timestamp(&s_cfg, &reference);
                } else {
                    ok = auth_set_current_timestamp(&s_cfg, cmd->num_param);
                }
            } else if (cmd->type == FIDO_CMD_ADD_TOTP || cmd->type == FIDO_CMD_EDIT_TOTP) {
                auth_totp_account_t account = {0};
                bool edit = cmd->type == FIDO_CMD_EDIT_TOTP;
                if (strlen(cmd->param1) < sizeof(account.issuer) && strlen(cmd->param2) < sizeof(account.account) &&
                    (!edit || cmd->num_param < s_cfg.account_count)) {
                    strcpy(account.issuer, cmd->param1);
                    strcpy(account.account, cmd->param2);
                    if (edit && !cmd->param3[0]) {
                        memcpy(account.secret, s_cfg.accounts[cmd->num_param].secret, sizeof(account.secret));
                    } else {
                        strcpy(account.secret, cmd->param3);
                    }
                    account.algorithm = cmd->otp_algorithm;
                    account.digits = cmd->otp_digits ? cmd->otp_digits : 6;
                    account.period = cmd->otp_period ? cmd->otp_period : 30;
                    ok = edit ? auth_storage_update_account(&s_cfg, (uint8_t)cmd->num_param, &account) :
                                auth_storage_add_account_ex(&s_cfg, &account);
                }
                volatile uint8_t *p = (volatile uint8_t *)&account;
                for (size_t i = 0; i < sizeof(account); i++) p[i] = 0;
            } else if (cmd->type == FIDO_CMD_DEL_TOTP) {
                ok = cmd->num_param < s_cfg.account_count && auth_storage_remove_account(&s_cfg, (uint8_t)cmd->num_param);
            } else if (cmd->type == FIDO_CMD_SET_BRIGHTNESS && cmd->num_param >= 10 && cmd->num_param <= 100) {
                uint8_t old = s_cfg.brightness;
                s_cfg.brightness = (uint8_t)cmd->num_param;
                ok = save_settings();
                if (!ok) s_cfg.brightness = old;
                bsp_display_backlight(s_cfg.brightness);
            } else if (cmd->type == FIDO_CMD_SET_SOUND && cmd->num_param <= 1) {
                bool old = s_cfg.sound_enabled;
                s_cfg.sound_enabled = cmd->num_param != 0;
                ok = save_settings();
                if (!ok) s_cfg.sound_enabled = old;
                if (ok && !s_cfg.sound_enabled) audio_off_now();
            } else if (cmd->type == FIDO_CMD_RESET) {
                s_settings_idx = AUTH_SETTINGS_WIPE_INDEX;
                auth_enter_view(AUTH_VIEW_SETTINGS);
                s_wipe_confirm = true;
                ok = true;
            }
            if (ok) sound_click();
            refresh_display();
            auth_wifi_command_result(cmd->request_id, ok);
            volatile uint8_t *p = (volatile uint8_t *)&evt.data.fido_cmd;
            for (size_t i = 0; i < sizeof(evt.data.fido_cmd); i++) p[i] = 0;
            continue;
        }

        if (evt.type == EVT_KEY) {
            if (evt.data.key.when_ms <= s_ignore_key_before_ms) continue;
            if (s_quartz_blank) {
                quartz_standby_wake();
                refresh_display();
                continue;
            }
            if (s_quartz_wake_guard) continue;
            s_last_activity_ms = (uint32_t)now_ms;
            bsp_btn_t btn = evt.data.key.btn;
            bsp_btn_ev_t kev = evt.data.key.ev;
            auth_view_t view = auth_ui_get_view();

            /* --- 1. Lock Screen Handling --- */
            if (view == AUTH_VIEW_LOCK) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP) {
                        passcode_guard_input_bit(&s_guard, true, now_ms);
                        snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "Press [OK] to unlock");
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        passcode_guard_input_bit(&s_guard, false, now_ms);
                        snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "Press [OK] to unlock");
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        if (s_cfg.auto_wipe_enabled && s_guard.failed_attempts > PASSCODE_WIPE_ATTEMPTS) {
                            bool wiped = finish_automatic_wipe();
                            snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "%s",
                                     wiped ? "TOKENS WIPED" : "WIPE INCOMPLETE");
                            sound_fail();
                            refresh_display();
                            continue;
                        }
                        uint8_t previous_failures = s_guard.failed_attempts;
                        passcode_result_t res = passcode_guard_submit(&s_guard, now_ms);
                        if (s_guard.failed_attempts != previous_failures &&
                            !auth_storage_pin_failures_save(s_guard.failed_attempts)) {
                            if (res == PASSCODE_RESULT_SUCCESS) s_guard.failed_attempts = previous_failures;
                            s_guard.state = PASSCODE_STATE_LOCKOUT;
                            s_guard.lockout_end_ms = now_ms + PASSCODE_LOCKOUT_MS;
                            s_rtc_failed_attempts = s_guard.failed_attempts;
                            s_rtc_lockout_end_us = (int64_t)esp_rtc_get_time_us() +
                                                   (int64_t)PASSCODE_LOCKOUT_MS * 1000LL;
                            snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "Retry count save failed");
                            sound_fail();
                            refresh_display();
                            continue;
                        }
                        s_rtc_failed_attempts = s_guard.failed_attempts;
                        s_rtc_lockout_end_us = s_guard.state == PASSCODE_STATE_LOCKOUT && s_guard.lockout_end_ms > now_ms
                            ? (int64_t)esp_rtc_get_time_us() + (int64_t)(s_guard.lockout_end_ms - now_ms) * 1000LL : 0;
                        if (res == PASSCODE_RESULT_SUCCESS) {
                            s_session_unlocked = true;
                            snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "UNLOCKED");
                            sound_success();
                            auth_enter_view(AUTH_VIEW_TOTP);
                        } else if (res == PASSCODE_RESULT_WIPED) {
                            s_session_unlocked = false;
                            if (s_cfg.auto_wipe_enabled) {
                                bool wiped = finish_automatic_wipe();
                                snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "%s",
                                         wiped ? "TOKENS WIPED" : "WIPE INCOMPLETE");
                            } else {
                                snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "PIN errors: %u", s_guard.failed_attempts);
                            }
                            sound_fail();
                        } else if (res == PASSCODE_RESULT_LOCKED_OUT) {
                            sound_fail();
                            snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "LOCKED OUT (%u errors)", s_guard.failed_attempts);
                        } else if (res == PASSCODE_RESULT_FAILED) {
                            sound_fail();
                            snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "WRONG PIN! (%u errors)", s_guard.failed_attempts);
                        }
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    passcode_guard_clear(&s_guard);
                    snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "Input Cleared");
                    sound_click();
                }
                refresh_display();
            }
            /* --- 2. TOTP Main View Handling --- */
            else if (view == AUTH_VIEW_TOTP) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP) {
                        if (s_cfg.account_count > 0) {
                            s_cur_acc_idx = auth_ui_totp_navigate(s_cfg.account_count, s_cur_acc_idx, s_totp_view, false);
                        }
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        if (s_cfg.account_count > 0) {
                            s_cur_acc_idx = auth_ui_totp_navigate(s_cfg.account_count, s_cur_acc_idx, s_totp_view, true);
                        }
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        s_settings_idx = 0;
                        s_wipe_confirm = false;
                        sound_click();
                        auth_enter_view(AUTH_VIEW_SETTINGS);
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    s_session_unlocked = false;
                    passcode_guard_lock(&s_guard);
                    snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "Press [OK] to unlock");
                    sound_click();
                    auth_enter_view(AUTH_VIEW_LOCK);
                }
                refresh_display();
            }
            /* --- 3. FIDO2 Prompt Handling --- */
            else if (view == AUTH_VIEW_FIDO_PROMPT) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_OK) {
                        fido_session_approve(&s_fido);
                        auth_ble_fido_user_confirm(true);
                        auth_ble_send_response("UP_APPROVED");
                        ui_lock();
                        auth_ui_update_fido_result(true);
                        ui_unlock();
                        sound_success();
                        vTaskDelay(pdMS_TO_TICKS(800));
                        s_ignore_key_before_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
                        auth_enter_view(s_view_before_modal);
                    } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                        fido_session_reject(&s_fido);
                        auth_ble_fido_user_confirm(false);
                        auth_ble_send_response("UP_REJECTED");
                        ui_lock();
                        auth_ui_update_fido_result(false);
                        ui_unlock();
                        sound_fail();
                        vTaskDelay(pdMS_TO_TICKS(800));
                        s_ignore_key_before_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
                        auth_enter_view(s_view_before_modal);
                    }
                }
                refresh_display();
            }
            /* --- 4. Settings View Handling --- */
            else if (view == AUTH_VIEW_SETTINGS) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP) {
                        s_settings_idx = (s_settings_idx == 0) ? (AUTH_SETTINGS_ITEM_COUNT - 1) : (s_settings_idx - 1);
                        s_wipe_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        s_settings_idx = (s_settings_idx + 1) % AUTH_SETTINGS_ITEM_COUNT;
                        s_wipe_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        uint8_t ui_idx = s_settings_idx;
                        s_settings_idx = settings_action_for_index(ui_idx);
                        if (s_settings_idx == 0) {
                            /* Toggle Brightness: 20% -> 40% -> 60% -> 80% -> 100% -> 20% */
                                s_cfg.brightness = s_cfg.brightness >= 100 ? 20 : (uint8_t)((s_cfg.brightness / 20 + 1) * 20);
                            bsp_display_backlight(s_cfg.brightness);
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 1) {
                            /* Toggle Sound */
                            s_cfg.sound_enabled = !s_cfg.sound_enabled;
                            if (save_settings() && !s_cfg.sound_enabled) audio_off_now();
                            sound_click();
                        } else if (s_settings_idx == 2) {
                            /* Cycle Auto Sleep Timeout: 15s -> 30s -> 60s -> 120s -> 300s -> Never */
                            for (size_t o = 0; o < SLEEP_TIMEOUT_OPTIONS_COUNT; o++) {
                                if (s_cfg.auto_sleep_sec == s_sleep_timeout_options[o]) {
                                    s_cfg.auto_sleep_sec = s_sleep_timeout_options[(o + 1) % SLEEP_TIMEOUT_OPTIONS_COUNT];
                                    break;
                                }
                            }
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 3) {
                            /* Toggle Auto-Wipe on >8 failed attempts */
                            s_cfg.auto_wipe_enabled = !s_cfg.auto_wipe_enabled;
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 4) {
                            /* Manual Wipe All Tokens: two-step confirmation */
                            if (!s_wipe_confirm) {
                                s_wipe_confirm = true;
                                sound_click();
                            } else {
                                bool wiped = erase_credentials(true);
                                fido_session_init(&s_fido);
                                s_cur_acc_idx = 0;
                                s_wipe_confirm = false;
                                if (wiped) {
                                    sound_success();
                                } else {
                                    sound_fail();
                                }
                            }
                        } else if (s_settings_idx == 5) {
                            /* Sleep Device Now */
                            sound_click();
                            vTaskDelay(pdMS_TO_TICKS(150));
                            system_enter_deep_sleep();
                        } else if (s_settings_idx == 6) {
                            /* Change Passcode */
                            s_change_pin_step = 0;
                            passcode_guard_start_change(&s_guard);
                            snprintf(s_cp_msg_buf, sizeof(s_cp_msg_buf), "[OK] Next | Long [OK] Cancel");
                            s_cp_is_error = false;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_CHANGE_PASSCODE);
                        } else if (s_settings_idx == 7) {
                            s_wifi_idx = 0;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_WIFI);
                        } else if (s_settings_idx == 8) {
                            /* Open Date & Time Adjustment */
                            uint64_t current_unix = auth_get_current_timestamp(&s_cfg);
                            int64_t local_sec = (int64_t)current_unix + (int64_t)s_cfg.timezone_offset * 3600LL;
                            if (local_sec < 0) local_sec = 0;
                            time_t t_local = (time_t)local_sec;
                            struct tm tm_buf;
                            gmtime_r(&t_local, &tm_buf);
                            s_set_year = tm_buf.tm_year + 1900;
                            if (s_set_year < 2024) s_set_year = 2026;
                            s_set_mon  = tm_buf.tm_mon + 1;
                            s_set_day  = tm_buf.tm_mday;
                            s_set_hour = tm_buf.tm_hour;
                            s_set_min  = tm_buf.tm_min;
                            s_set_sec  = tm_buf.tm_sec;
                            s_set_time_field = 0;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_SET_TIME);
                        } else if (s_settings_idx == 9) {
                            /* Toggle Time Format (24h / 12h) */
                            s_cfg.time_format_24h = !s_cfg.time_format_24h;
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 10) {
                            /* Cycle Timezone Offset */
                            size_t cur_tz_idx = 0;
                            for (size_t t = 0; t < TZ_OPTIONS_COUNT; t++) {
                                if (s_cfg.timezone_offset == s_tz_options[t]) {
                                    cur_tz_idx = t;
                                    break;
                                }
                            }
                            s_cfg.timezone_offset = s_tz_options[(cur_tz_idx + 1) % TZ_OPTIONS_COUNT];
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 11) {
                            /* Toggle Battery Protect (80%) */
                            s_cfg.battery_protect_80 = !s_cfg.battery_protect_80;
                            save_settings();
                            sound_click();
                        } else if (s_settings_idx == 12) {
                            /* Exit to TOTP */
                            sound_click();
                            auth_enter_view(AUTH_VIEW_TOTP);
                        } else if (s_settings_idx == 13) {
                            /* Lab / Experiments */
                            s_lab_idx = 0;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_LAB);
                        } else if (s_settings_idx == 14) {
                            auth_totp_view_t next = auth_ui_next_totp_view(s_totp_view);
                            if (auth_storage_totp_view_save(next)) {
                                s_totp_view = next;
                                sound_click();
                            } else sound_fail();
                        } else if (s_settings_idx == 15) {
                            s_clock_mode_idx = (uint8_t)rtc_clock_get_mode();
                            s_clock_mode_page = 0;
                            s_clock_mode_confirm = false;
                            s_clock_mode_save_failed = false;
                            auth_enter_view(AUTH_VIEW_CLOCK_MODES);
                        }
                        s_settings_idx = ui_idx;
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    sound_click();
                    s_wipe_confirm = false;
                    auth_enter_view(AUTH_VIEW_TOTP);
                }
                refresh_display();
            }
            /* --- 4b. Lab / Experiments Handling --- */
            else if (view == AUTH_VIEW_CLOCK_MODES) {
                if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    if (s_clock_mode_page) s_clock_mode_page = 0;
                    else auth_enter_view(AUTH_VIEW_SETTINGS);
                    s_clock_mode_confirm = false;
                    s_clock_mode_save_failed = false;
                } else if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                        const bool forward = btn == BSP_BTN_DOWN;
                        s_clock_mode_confirm = false;
                        s_clock_mode_save_failed = false;
                        if (!s_clock_mode_page) {
                            s_clock_mode_idx = auth_ui_clock_modes_next_selection(s_clock_mode_idx, forward);
                        } else {
                            s_clock_mode_page = auth_ui_clock_modes_next_page(s_clock_mode_page, forward);
                        }
                    } else if (btn == BSP_BTN_OK) {
                        if (!s_clock_mode_page) {
                            if (s_clock_mode_idx == RTC_MODE_COUNT) auth_enter_view(AUTH_VIEW_SETTINGS);
                            else s_clock_mode_page = 1;
                        } else if (s_clock_mode_page < 4) {
                            ++s_clock_mode_page;
                        } else if (s_clock_mode_idx == (uint8_t)rtc_clock_get_mode()) {
                            s_clock_mode_page = 0;
                        } else if (!s_clock_mode_confirm) {
                            s_clock_mode_confirm = true;
                        } else {
                            (void)clock_mode_apply((rtc_mode_t)s_clock_mode_idx);
                            s_clock_mode_confirm = false;
                        }
                    }
                }
                refresh_display();
            }
            else if (view == AUTH_VIEW_WIFI) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                        s_wifi_idx ^= 1;
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        if (s_wifi_idx == 1) {
                            sound_click();
                            auth_enter_view(AUTH_VIEW_SETTINGS);
                        } else if (toggle_wifi_hotspot()) {
                            sound_click();
                        } else {
                            sound_fail();
                        }
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    sound_click();
                    auth_enter_view(AUTH_VIEW_SETTINGS);
                }
                refresh_display();
            }
            else if (view == AUTH_VIEW_LAB) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP) {
                        s_lab_idx = (s_lab_idx == 0) ? (AUTH_LAB_ITEM_COUNT - 1) : (s_lab_idx - 1);
                        s_fido_reset_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        s_lab_idx = (s_lab_idx + 1) % AUTH_LAB_ITEM_COUNT;
                        s_fido_reset_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        if (s_lab_idx == 0) {
                            /* BLE FIDO transport toggle */
                            if (!s_lab_cfg.ble_fido_enabled) {
                                bool wifi_was_active = auth_wifi_ap_is_active();
                                if (wifi_was_active && !auth_wifi_ap_stop()) {
                                    ESP_LOGE(TAG, "Wi-Fi shutdown incomplete; refusing BLE start");
                                    sound_fail();
                                    refresh_display();
                                    continue;
                                }
                                if (auth_ble_init(on_ble_cmd)) {
                                    auth_ble_start_advertising();
                                    s_lab_cfg.ble_fido_enabled = true;
                                    auth_storage_lab_save(&s_lab_cfg);
                                    sound_success();
                                } else {
                                    if (wifi_was_active) auth_wifi_ap_start(&s_cfg, on_wifi_cmd);
                                    sound_fail();
                                }
                            } else {
                                if (auth_ble_stop()) {
                                    s_lab_cfg.ble_fido_enabled = false;
                                    if (!auth_storage_lab_save(&s_lab_cfg)) ESP_LOGE(TAG, "BLE preference could not be saved");
                                    sound_click();
                                } else {
                                    sound_fail();
                                }
                            }
                        } else if (s_lab_idx == 1) {
                            /* Reversible app-log console toggle (not hardware JTAG) */
                            s_lab_cfg.console_log_enabled = !s_lab_cfg.console_log_enabled;
                            auth_storage_lab_save(&s_lab_cfg);
                            if (s_lab_cfg.console_log_enabled) {
                                esp_log_level_set("*", ESP_LOG_INFO);
                                ESP_LOGI(TAG, "USB log console enabled");
                            } else {
                                ESP_LOGI(TAG, "USB log console disabled");
                                esp_log_level_set("*", ESP_LOG_NONE);
                            }
                            sound_click();
                        } else if (s_lab_idx == 2) {
                            s_fido_rp_count = auth_storage_fido_list(s_fido_rps, AUTH_MAX_FIDO_RPS);
                            s_fido_acc_idx = 0;
                            s_fido_acc_confirm = false;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_FIDO_ACCOUNTS);
                        } else if (s_lab_idx == 3) {
                            /* Reset FIDO: two-step confirmation */
                            if (!s_fido_reset_confirm) {
                                s_fido_reset_confirm = true;
                                sound_click();
                            } else {
                                bool reset = erase_credentials(false);
                                s_fido_reset_confirm = false;
                                if (reset) sound_success();
                                else sound_fail();
                            }
                        } else if (s_lab_idx == 4) {
                            s_fido_reset_confirm = false;
                            sound_click();
                            auth_enter_view(AUTH_VIEW_SETTINGS);
                        }
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    s_fido_reset_confirm = false;
                    sound_click();
                    auth_enter_view(AUTH_VIEW_SETTINGS);
                }
                refresh_display();
            }
            /* --- 4c. FIDO Credential Management --- */
            else if (view == AUTH_VIEW_FIDO_ACCOUNTS) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP && s_fido_rp_count > 0) {
                        s_fido_acc_idx = (s_fido_acc_idx == 0) ? (s_fido_rp_count - 1) : (s_fido_acc_idx - 1);
                        s_fido_acc_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN && s_fido_rp_count > 0) {
                        s_fido_acc_idx = (s_fido_acc_idx + 1) % s_fido_rp_count;
                        s_fido_acc_confirm = false;
                        sound_click();
                    } else if (btn == BSP_BTN_OK && s_fido_rp_count > 0) {
                        if (s_fido_rps[s_fido_acc_idx].legacy) {
                            s_fido_acc_confirm = false;
                            sound_fail();
                        } else if (!s_fido_acc_confirm) {
                            s_fido_acc_confirm = true;
                            sound_click();
                        } else {
                            if (auth_storage_fido_delete(s_fido_rps[s_fido_acc_idx].rp_id)) {
                                s_fido_rp_count = auth_storage_fido_list(s_fido_rps, AUTH_MAX_FIDO_RPS);
                                if (s_fido_rp_count == 0) {
                                    s_fido_acc_idx = 0;
                                } else if (s_fido_acc_idx >= s_fido_rp_count) {
                                    s_fido_acc_idx = s_fido_rp_count - 1;
                                }
                                s_fido_acc_confirm = false;
                                sound_success();
                            } else {
                                sound_fail();
                            }
                        }
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    s_fido_acc_confirm = false;
                    sound_click();
                    auth_enter_view(AUTH_VIEW_LAB);
                }
                refresh_display();
            }
            /* --- 5. Change Passcode View Handling --- */
            else if (view == AUTH_VIEW_CHANGE_PASSCODE) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_UP) {
                        passcode_guard_input_bit(&s_guard, true, now_ms);
                        s_cp_is_error = false;
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        passcode_guard_input_bit(&s_guard, false, now_ms);
                        s_cp_is_error = false;
                        sound_click();
                    } else if (btn == BSP_BTN_OK) {
                        passcode_result_t res = passcode_guard_submit(&s_guard, now_ms);
                        if (res == PASSCODE_RESULT_INPUT_UPDATED) {
                            /* Step 1 finished, enter Step 2 (confirm) */
                            s_change_pin_step = 1;
                            snprintf(s_cp_msg_buf, sizeof(s_cp_msg_buf), "[OK] Confirm | Long [OK] Back");
                            s_cp_is_error = false;
                            sound_click();
                        } else if (res == PASSCODE_RESULT_FAILED) {
                            /* Step 1 failed because length < 4 */
                            snprintf(s_cp_msg_buf, sizeof(s_cp_msg_buf), "PIN TOO SHORT! Min 4 bits");
                            s_cp_is_error = true;
                            sound_fail();
                        } else if (res == PASSCODE_RESULT_NEW_SET) {
                            /* Successfully set new PIN! */
                            s_cfg.passcode_pattern = s_guard.target_pattern;
                            s_cfg.passcode_len = s_guard.target_len;
                            if (save_settings()) sound_success();
                            auth_enter_view(AUTH_VIEW_SETTINGS);
                        } else if (res == PASSCODE_RESULT_MISMATCH) {
                            snprintf(s_cp_msg_buf, sizeof(s_cp_msg_buf), "MISMATCH! PIN not changed");
                            s_cp_is_error = true;
                            sound_fail();
                            refresh_display();
                            vTaskDelay(pdMS_TO_TICKS(1200));
                            s_ignore_key_before_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
                            auth_enter_view(AUTH_VIEW_SETTINGS);
                            continue;
                        }
                    }
                } else if (kev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                    if (s_guard.current_len > 0) {
                        passcode_guard_clear(&s_guard);
                        s_cp_is_error = false;
                        sound_click();
                    } else {
                        sound_click();
                        auth_enter_view(AUTH_VIEW_SETTINGS);
                    }
                }
                refresh_display();
            }
            /* --- 6. Set Date & Time View Handling --- */
            else if (view == AUTH_VIEW_SET_TIME) {
                if (kev == BSP_BTN_CLICK) {
                    if (btn == BSP_BTN_OK) {
                        s_set_time_field = (s_set_time_field + 1) % 6;
                        sound_click();
                    } else if (btn == BSP_BTN_UP) {
                        if (s_set_time_field == 0) {
                            s_set_year = (s_set_year >= 2099) ? 2024 : (s_set_year + 1);
                        } else if (s_set_time_field == 1) {
                            s_set_mon = (s_set_mon >= 12) ? 1 : (s_set_mon + 1);
                        } else if (s_set_time_field == 2) {
                            int max_d = days_in_month(s_set_year, s_set_mon);
                            s_set_day = (s_set_day >= max_d) ? 1 : (s_set_day + 1);
                        } else if (s_set_time_field == 3) {
                            s_set_hour = (s_set_hour + 1) % 24;
                        } else if (s_set_time_field == 4) {
                            s_set_min = (s_set_min + 1) % 60;
                        } else if (s_set_time_field == 5) {
                            s_set_sec = (s_set_sec + 1) % 60;
                        }
                        sound_click();
                    } else if (btn == BSP_BTN_DOWN) {
                        if (s_set_time_field == 0) {
                            s_set_year = (s_set_year <= 2024) ? 2099 : (s_set_year - 1);
                        } else if (s_set_time_field == 1) {
                            s_set_mon = (s_set_mon <= 1) ? 12 : (s_set_mon - 1);
                        } else if (s_set_time_field == 2) {
                            int max_d = days_in_month(s_set_year, s_set_mon);
                            s_set_day = (s_set_day <= 1) ? max_d : (s_set_day - 1);
                        } else if (s_set_time_field == 3) {
                            s_set_hour = (s_set_hour == 0) ? 23 : (s_set_hour - 1);
                        } else if (s_set_time_field == 4) {
                            s_set_min = (s_set_min == 0) ? 59 : (s_set_min - 1);
                        } else if (s_set_time_field == 5) {
                            s_set_sec = (s_set_sec == 0) ? 59 : (s_set_sec - 1);
                        }
                        sound_click();
                    }
                } else if (kev == BSP_BTN_LONG) {
                    if (btn == BSP_BTN_OK) {
                        int64_t target_utc = calculate_utc_epoch(s_set_year, s_set_mon, s_set_day,
                                                                 s_set_hour, s_set_min, s_set_sec,
                                                                 s_cfg.timezone_offset);
                        if (auth_set_current_timestamp(&s_cfg, (uint64_t)target_utc)) sound_success();
                        else sound_fail();
                        auth_enter_view(AUTH_VIEW_SETTINGS);
                    } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                        sound_click();
                        auth_enter_view(AUTH_VIEW_SETTINGS);
                    }
                }
                refresh_display();
            }
            if (s_settings_write_failed) sound_fail();
        }
    }
}

static void init_power_management(void)
{
#if CONFIG_PM_ENABLE
    const bool light_ready = rtc_mode_light_sleep(rtc_clock_get_mode()) &&
                             rtc_clock_start_light_sleep();
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = light_ready,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK && light_ready) {
        rtc_light_sleep_platform_inhibit();
        ESP_LOGW(TAG, "Automatic light sleep unavailable: %s; retrying DFS", esp_err_to_name(err));
        pm_config.light_sleep_enable = false;
        err = esp_pm_configure(&pm_config);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PMU DFS active (40-160MHz), automatic light sleep=%u",
                 (unsigned)pm_config.light_sleep_enable);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(err));
    }
#endif
}

void app_main(void)
{
    const bool clock_deep_wakeup = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    const uint32_t early_dig_hold = REG_READ(RTC_CNTL_DIG_PAD_HOLD_REG);
    const uint32_t early_dig_iso = REG_READ(RTC_CNTL_DIG_ISO_REG);
    const uint32_t early_io21_mux = REG_READ(IO_MUX_GPIO21_REG);
    key_guard_arm(&s_key_guard);
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    const bool timer_only = clock_deep_wakeup && wake_causes == (1U << ESP_SLEEP_WAKEUP_TIMER);
    bsp_display_backlight_early_off();
    const bool observer_ready = timer_only && clock_key_observer_begin();
    esp_deep_sleep_disable_rom_logging();

    /* 0. NVS Flash initialization */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        clock_key_observer_end();
        ESP_LOGE(TAG, "NVS initialization failed; stored credentials were not erased: %s", esp_err_to_name(nvs_err));
        return;
    }

    rtc_mode_t clock_mode = RTC_MODE_ECO;
    if (!auth_storage_clock_mode_load(&clock_mode)) {
        ESP_LOGW(TAG, "Clock preference unavailable; using Eco without overwriting it");
    }
    if (!rtc_clock_configure_mode(clock_mode)) {
        clock_key_observer_end();
        ESP_LOGE(TAG, "Clock mode configuration failed");
        return;
    }

    // 先修正深睡的 UTC 贡献，再初始化慢外设；PIN 的 RTC/HRT 单调计时不重置。
    auth_time_init(clock_deep_wakeup);
    const rtc_clock_boot_result_t clock_result = rtc_clock_boot(clock_deep_wakeup, auth_time_is_set());
    if (clock_result == RTC_CLOCK_NEEDS_SYNC) {
        auth_time_init(false);
        ESP_LOGW(TAG, "RTC continuation untrusted; time synchronization required");
    }
    if (timer_only) {
        bsp_display_hold_sleep_levels();
    }
    clock_maintenance_try(timer_only, observer_ready);
    clock_maintenance_quiet_fallback(timer_only, observer_ready);
    clock_key_observer_end();
    rtc_clock_begin_normal_wake();

    auth_storage_lab_load(&s_lab_cfg);
    esp_log_level_set("*", s_lab_cfg.console_log_enabled ? ESP_LOG_INFO : ESP_LOG_NONE);

    ESP_LOGI(TAG, "Starting FoloToy AI Passport FIDO8TOTP Authenticator...");
    ESP_LOGI(TAG, "Reset reason: %d, free heap: %u",
             (int)esp_reset_reason(), (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Early wake hold: causes=0x%lx dig_hold=0x%08lx dig_iso=0x%08lx io21_mux=0x%08lx",
             (unsigned long)wake_causes, (unsigned long)early_dig_hold,
             (unsigned long)early_dig_iso, (unsigned long)early_io21_mux);

    /* 启动数值诊断：检查点不是完整睡眠或全路径盲区实测。 */
    const rtc_clock_diagnostics_t clock_diag = rtc_clock_diagnostics();
    ESP_LOGI(TAG, "RTC_CAL restored=%u checkpoint_us=%llu period_q19=%u samples=%u pre_tail_gap_upper_us=%u fault=%u user=%u blocked=%u model_n=%u model_rms_millippm=%u",
             (unsigned)clock_diag.utc_restored, (unsigned long long)clock_diag.checkpoint_elapsed_us,
             (unsigned)clock_diag.period_q19, (unsigned)clock_diag.samples,
             (unsigned)clock_diag.pre_tail_gap_upper_us, (unsigned)clock_diag.fault,
             (unsigned)clock_diag.user_wake, (unsigned)clock_diag.maintenance_blocked,
             (unsigned)clock_diag.model_observations, (unsigned)clock_diag.model_rms_millippm);
    const rtc_clock_history_summary_t history_diag = rtc_clock_history_owner_summary();
    ESP_LOGI(TAG, "RTC_HISTORY source=%u ready=%u status=%u rows=%lu pending=%lu ppm=%ld qualified=%u reason=%lu",
             (unsigned)clock_diag.source, (unsigned)history_diag.ready, (unsigned)history_diag.store_status,
             (unsigned long)history_diag.durable_count, (unsigned long)history_diag.pending_count,
             (long)clock_diag.ppm, (unsigned)clock_diag.ppm_qualified, (unsigned long)clock_diag.learning_reason);
    init_power_management();

    /* 1. Shared I2C Bus */
    ESP_ERROR_CHECK(bsp_i2c_init());

    /* 2. Battery Fuel Gauge */
    bsp_battery_init();

    /* 3. Display */
    ESP_ERROR_CHECK(bsp_display_init());

    /* 4. Storage & NVS */
    if (!auth_storage_init(&s_cfg)) {
        ESP_LOGE(TAG, "Cannot load or migrate account storage; original data was preserved");
        return;
    }
    if (!auth_storage_totp_view_load(&s_totp_view))
        ESP_LOGW(TAG, "Display preference unavailable; using card view");
    if (!auth_wifi_init_identity()) {
        ESP_LOGE(TAG, "Unable to read chip identity; Wi-Fi AP will remain unavailable");
    }

    /* 5. Audio */
    if (bsp_audio_init() == ESP_OK) {
        bsp_audio_set_format(16000, 16, 1);
        bsp_audio_set_volume(80);
        if (s_cfg.sound_enabled) audio_standby_now();
        else audio_off_now();
    }

    /* 7. Passcode Guard & FIDO Session */
    passcode_guard_init(&s_guard, s_cfg.passcode_pattern, s_cfg.passcode_len);
    uint8_t saved_failures = 0;
    if (!auth_storage_pin_failures_load(&saved_failures)) {
        ESP_LOGE(TAG, "Cannot load PIN retry count");
        return;
    }
    bool deep_wakeup = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    if (deep_wakeup && s_rtc_failed_attempts > saved_failures) saved_failures = s_rtc_failed_attempts;
    int64_t now_rtc_us = (int64_t)esp_rtc_get_time_us();
    uint32_t remaining_lockout = 0;
    if (saved_failures >= PASSCODE_MAX_ATTEMPTS) {
        if (deep_wakeup && s_rtc_lockout_end_us > now_rtc_us)
            remaining_lockout = (uint32_t)((s_rtc_lockout_end_us - now_rtc_us) / 1000ULL);
        else if (!deep_wakeup) remaining_lockout = PASSCODE_LOCKOUT_MS;
    }
    passcode_guard_restore_attempts(&s_guard, saved_failures,
                                    (uint64_t)(esp_timer_get_time() / 1000ULL), remaining_lockout);
    s_rtc_failed_attempts = saved_failures;
    s_rtc_lockout_end_us = remaining_lockout ? now_rtc_us + (int64_t)remaining_lockout * 1000LL : 0;
    if (saved_failures) snprintf(s_lock_msg_buf, sizeof(s_lock_msg_buf), "PIN errors: %u", saved_failures);
    fido_session_init(&s_fido);

    /* 8. Event Queue (32 entries) */
    s_evt_queue = xQueueCreate(32, sizeof(app_event_t));
    if (!s_evt_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        esp_restart();
    }

    /* 9. 所有启动路径都先隔离已有手势；初始化后仅由按键定时器确认整板空闲。 */
    ESP_ERROR_CHECK(bsp_button_init(on_key, NULL));
    s_input_ready = true;

    /* 10. LVGL Initialization & UI Creation */
    if (!bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Failed to initialize LVGL");
        esp_restart();
    }
    ui_lock();
    auth_ui_init();
    ui_unlock();
    refresh_display();
    lv_mem_monitor_t ui_memory;
    ui_lock();
    lv_mem_monitor(&ui_memory);
    lv_refr_now(NULL);
    ui_unlock();
    ESP_LOGI(TAG, "LVGL pool free=%u largest=%u", (unsigned)ui_memory.free_size,
             (unsigned)ui_memory.free_biggest_size);

    /* 11. Wait briefly so the final SPI DMA chunk settles in ST7789 GRAM before lighting backlight */
    vTaskDelay(pdMS_TO_TICKS(20));
    bsp_display_backlight(s_cfg.brightness);

    /* 12. BLE Peripheral & FIDO GATT Service (opt-in via Lab) */
    if (s_lab_cfg.ble_fido_enabled) {
        if (!auth_ble_init(on_ble_cmd)) {
            ESP_LOGE(TAG, "BLE FIDO init failed");
        }
    } else {
        ESP_LOGI(TAG, "BLE FIDO disabled (enable in Lab / Experiments)");
    }

    /* 13. Worker Tasks */
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (xTaskCreate(auth_app_task, "auth_app", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auth application task");
        esp_restart();
    }
    if (xTaskCreate(auth_tick_task, "auth_tick", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auth tick task");
        esp_restart();
    }

    ESP_LOGI(TAG, "FIDO8TOTP Authenticator initialized successfully (free heap: %u)",
             (unsigned)esp_get_free_heap_size());
}
