/* 界面接口：纯格式化/分页函数可在主机测试，其余接口由调用方持有 BSP LVGL 锁。
 * 卡片和列表复用控件，菜单只保留当前分组；提示文字不得覆盖按键导航底栏。
 */
#ifndef AUTH_UI_H
#define AUTH_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "auth_storage.h"
#include "fido_protocol.h"
#include "passcode_guard.h"
#include "rtc_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTH_VIEW_LOCK = 0,
    AUTH_VIEW_TOTP,
    AUTH_VIEW_FIDO_PROMPT,
    AUTH_VIEW_FIDO_RESULT,
    AUTH_VIEW_SETTINGS,
    AUTH_VIEW_CHANGE_PASSCODE,
    AUTH_VIEW_SET_TIME,
    AUTH_VIEW_LAB,
    AUTH_VIEW_WIFI,
    AUTH_VIEW_FIDO_ACCOUNTS,
    AUTH_VIEW_CLOCK_MODES,
} auth_view_t;

uint32_t auth_ui_battery_color(int8_t soc, bool charging, bool protection);
void auth_ui_format_top_clock(char *out, size_t out_size, uint64_t current_unix,
                              int8_t timezone_offset, bool format_24h);
void auth_ui_update_top_clock(uint64_t current_unix, int8_t timezone_offset, bool format_24h);
void auth_ui_update_wifi(uint8_t selected_idx, bool active, const char *ssid, const char *password);
/**
 * @brief Initialize all UI screens and objects. Must be called while holding LVGL lock.
 */
void auth_ui_init(void);

/**
 * @brief Switch active view.
 */
void auth_ui_set_view(auth_view_t view);

/**
 * @brief Get currently active view.
 */
auth_view_t auth_ui_get_view(void);

/**
 * @brief Update Top Status Bar (BLE status, Battery percentage, charging indicator).
 */
void auth_ui_update_status_bar(bool ble_conn, int8_t battery_soc, bool is_charging, bool battery_protect);

void auth_ui_format_battery(char *out, size_t out_size, int8_t soc,
                            bool charging, bool protection);

/**
 * @brief Update Lock Screen data (passcode and live clock/date).
 */
void auth_ui_update_lock_screen(const char *input_str, uint8_t input_len,
                                const char *status_msg, bool is_lockout, uint32_t lockout_rem_sec,
                                int64_t current_unix, int8_t timezone_offset, bool format_24h);

/**
 * @brief Update TOTP view data.
 */
void auth_ui_update_totp(const auth_totp_account_t *account,
                         uint8_t cur_idx, uint8_t total_acc,
                         const char *code, uint32_t rem_sec, uint32_t period,
                         bool ble_conn, int8_t battery_soc);

/**
 * @brief Update FIDO2 User Presence Request prompt.
 */
void auth_ui_update_fido_prompt(const fido_request_t *req, uint32_t rem_sec);

/**
 * @brief Update FIDO2 result feedback.
 */
void auth_ui_update_fido_result(bool approved);

typedef struct {
    uint8_t first;
    uint8_t count;
    uint8_t group;
} auth_settings_window_t;

auth_settings_window_t auth_ui_settings_window(uint8_t selected);

#define AUTH_SETTINGS_ITEM_COUNT 16
#define AUTH_SETTINGS_WIPE_INDEX 7
#define AUTH_SETTINGS_CLOCK_MODES_INDEX 14
#define AUTH_SETTINGS_EXIT_INDEX 15
#define AUTH_TOTP_LIST_MAX 4

typedef struct {
    const auth_totp_account_t *account;
    const char *code;
    uint32_t remaining;
} auth_totp_list_item_t;

auth_totp_view_t auth_ui_next_totp_view(auth_totp_view_t mode);
uint8_t auth_ui_totp_page_start(uint8_t selected, auth_totp_view_t mode);
uint8_t auth_ui_totp_page_count(uint8_t total, uint8_t selected, auth_totp_view_t mode);
uint8_t auth_ui_totp_navigate(uint8_t total, uint8_t selected, auth_totp_view_t mode, bool forward);
void auth_ui_update_totp_list(const auth_totp_list_item_t *items, uint8_t first_index,
                              uint8_t count, uint8_t total, auth_totp_view_t mode);
#define AUTH_LAB_ITEM_COUNT 5

#define AUTH_CLOCK_MODES_BACK_INDEX RTC_MODE_COUNT
#define AUTH_CLOCK_MODES_ITEM_COUNT (RTC_MODE_COUNT + 1)
typedef enum {
    AUTH_CLOCK_MODES_PAGE_LIST = 0,
    AUTH_CLOCK_MODES_PAGE_BEHAVIOR = 1,
    AUTH_CLOCK_MODES_PAGE_STANDBY = 2,
    AUTH_CLOCK_MODES_PAGE_ACCURACY = 3,
    AUTH_CLOCK_MODES_PAGE_APPLY = 4,
} auth_clock_modes_page_t;

/* List navigation wraps over the three modes and Back. Invalid selection resets to 0.
 * Detail navigation wraps through pages 1..4; page 0 stays on the list.
 * Invalid detail pages reset to page 4 without stepping.
 */
uint8_t auth_ui_clock_modes_next_selection(uint8_t selected, bool forward);
uint8_t auth_ui_clock_modes_next_page(uint8_t page, bool forward);

/**
 * Render only; caller owns selection, pages, confirmation, persistence and restart.
 * On list OK opens page 1, or exits when Back is selected. UP/DOWN move list
 * selection or detail pages. On pages 1..3, OK may advance to the next page.
 * Page 4 OK arms confirmation; the next OK saves/restarts only for an inactive mode.
 * Long OK returns any detail/confirmation to the list, then list to Settings.
 * Moving pages cancels confirmation/save_failed. Active mode OK on page 4 returns to list.
 * save_failed is shown on page 4, retaining the warning and allowing an explicit retry.
 * Must hold the LVGL lock. Panel is allocated on entry and freed on leaving this view.
 */
void auth_ui_update_clock_modes(rtc_mode_t active, uint8_t selected, uint8_t page,
                                bool confirming, bool save_failed);

/**
 * @brief Update Settings menu.
 */
void auth_ui_update_settings(uint8_t selected_idx, uint8_t brightness,
                             bool sound_enabled, auth_totp_view_t otp_view, uint16_t auto_sleep_sec,
                             bool auto_wipe_enabled, uint8_t passcode_len,
                             bool wifi_ap_active,
                             uint64_t current_epoch, int8_t timezone_offset,
                             bool format_24h, bool battery_protect_80,
                             bool confirm_wipe);

/**
 * @brief Update Change Passcode screen.
 */
void auth_ui_update_change_passcode(uint8_t step, const char *input_str, uint8_t input_len,
                                    const char *msg, bool is_error);

/**
 * @brief Update Lab / Experiments menu.
 */
void auth_ui_update_lab(uint8_t selected_idx, bool ble_fido_enabled,
                        bool console_log_enabled, bool confirm_fido_reset);

/**
 * @brief Update FIDO credential list.
 */
void auth_ui_update_fido_accounts(const auth_fido_site_t *rps, uint8_t count,
                                  uint8_t selected_idx, bool confirm_delete);

/**
 * @brief Update the Wi-Fi indicator in the top status bar.
 */
void auth_ui_update_wifi_status(bool active);

/**
 * @brief Update BLE state: disabled, advertising, or connected.
 */
void auth_ui_update_ble_status(bool enabled, bool connected);

/**
 * @brief Update Date & Time Adjustment screen.
 */
void auth_ui_update_set_time(uint8_t active_field, int year, int month, int day,
                             int hour, int min, int sec);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_UI_H */
