/* 小内存界面：卡片与列表复用文字控件，设置、Lab 和 Wi-Fi 共用一套菜单面板。
 * 设置只显示当前分组三项，避免常驻长列表耗尽 32 KiB 控件池及字形临时缓冲。
 * 所有原生控件接口均由持有 BSP LVGL 锁的调用方使用，纯格式化函数可在主机测试。
 */
#include "auth_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void auth_ui_format_battery(char *out, size_t out_size, int8_t soc,
                            bool charging, bool protection)
{
    if (!out || out_size == 0) return;
    if (soc < 0) {
        snprintf(out, out_size, "--%s%s", protection && charging ? "*" : "", charging ? "+" : "");
    } else {
        snprintf(out, out_size, "%d%%%s%s", soc, protection && charging ? "*" : "", charging ? "+" : "");
    }
}

uint32_t auth_ui_battery_color(int8_t soc, bool charging, bool protection)
{
    if (soc <= 10) return 0xFF9B9B;
    if (soc <= 30) return 0xEDC776;
    if (!charging) return 0xA2B0C0;
    if (protection && soc >= 80) return 0x6AAEFF;
    return 0x80D6BD;
}

void auth_ui_format_top_clock(char *out, size_t out_size, uint64_t current_unix,
                              int8_t timezone_offset, bool format_24h)
{
    if (!out || out_size == 0) return;
    if (!current_unix) {
        snprintf(out, out_size, "--:--");
        return;
    }
    int64_t minutes = (int64_t)(current_unix / 60) + timezone_offset * 60;
    minutes %= 1440;
    if (minutes < 0) minutes += 1440;
    unsigned hour = (unsigned)(minutes / 60);
    unsigned minute = (unsigned)(minutes % 60);
    if (format_24h) {
        snprintf(out, out_size, "%02u:%02u", hour, minute);
    } else {
        unsigned display_hour = hour % 12;
        if (!display_hour) display_hour = 12;
        snprintf(out, out_size, "%02u:%02u %s", display_hour, minute, hour < 12 ? "AM" : "PM");
    }
}

auth_totp_view_t auth_ui_next_totp_view(auth_totp_view_t mode)
{
    if (mode == AUTH_TOTP_VIEW_CARD) return AUTH_TOTP_VIEW_LIST3;
    if (mode == AUTH_TOTP_VIEW_LIST3) return AUTH_TOTP_VIEW_LIST4;
    return AUTH_TOTP_VIEW_CARD;
}

uint8_t auth_ui_totp_page_start(uint8_t selected, auth_totp_view_t mode)
{
    uint8_t rows = mode == AUTH_TOTP_VIEW_LIST3 || mode == AUTH_TOTP_VIEW_LIST4 ? (uint8_t)mode : 1;
    return (uint8_t)(selected / rows * rows);
}

uint8_t auth_ui_totp_page_count(uint8_t total, uint8_t selected, auth_totp_view_t mode)
{
    if (selected >= total) return 0;
    uint8_t rows = mode == AUTH_TOTP_VIEW_LIST3 || mode == AUTH_TOTP_VIEW_LIST4 ? (uint8_t)mode : 1;
    uint8_t remaining = (uint8_t)(total - auth_ui_totp_page_start(selected, mode));
    return remaining < rows ? remaining : rows;
}

uint8_t auth_ui_totp_navigate(uint8_t total, uint8_t selected, auth_totp_view_t mode, bool forward)
{
    if (!total) return 0;
    if (selected >= total) selected = 0;
    uint8_t rows = mode == AUTH_TOTP_VIEW_LIST3 || mode == AUTH_TOTP_VIEW_LIST4 ? (uint8_t)mode : 1;
    uint8_t first = auth_ui_totp_page_start(selected, mode);
    if (forward) return first + rows < total ? (uint8_t)(first + rows) : 0;
    return first >= rows ? (uint8_t)(first - rows) : (uint8_t)((total - 1) / rows * rows);
}

auth_settings_window_t auth_ui_settings_window(uint8_t selected)
{
    static const uint8_t starts[] = {0, 3, 6, 9, 10, 13, 14, 15};
    for (uint8_t group = 0; group < 7; group++) {
        if (selected < starts[group + 1]) {
            return (auth_settings_window_t){starts[group], starts[group + 1] - starts[group], group};
        }
    }
    return (auth_settings_window_t){14, 1, 6};
}

#ifdef ESP_PLATFORM
#include "lvgl.h"

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) lv_label_set_text(label, text);
}

static auth_view_t s_current_view = AUTH_VIEW_LOCK;

/* Main containers */
static lv_obj_t *s_root_screen;
static lv_obj_t *s_top_bar;
static lv_obj_t *s_bottom_bar;

static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_ble_status;
static lv_obj_t *s_lbl_wifi_status;
static lv_obj_t *s_lbl_battery;
static lv_obj_t *s_lbl_footer_hint;

/* View panels */
static lv_obj_t *s_pnl_lock;
static lv_obj_t *s_pnl_totp;
static lv_obj_t *s_pnl_fido;
static lv_obj_t *s_pnl_change_pass;
static lv_obj_t *s_pnl_set_time;
static lv_obj_t *s_pnl_lab;
static lv_obj_t *s_pnl_fido_acc;

/* Lock view widgets */
static lv_obj_t *s_lbl_lock_clock;
static lv_obj_t *s_lbl_lock_date;
static lv_obj_t *s_lbl_lock_title;
static lv_obj_t *s_lbl_lock_input;
static lv_obj_t *s_lbl_lock_count;
static lv_obj_t *s_lbl_lock_msg;

/* TOTP view widgets */
static lv_obj_t *s_lbl_totp_index;
static lv_obj_t *s_lbl_totp_issuer;
static lv_obj_t *s_lbl_totp_account;
static lv_obj_t *s_lbl_totp_code;
static lv_obj_t *s_bar_totp_countdown;
static lv_obj_t *s_lbl_totp_seconds;

/* FIDO view widgets */
static lv_obj_t *s_lbl_fido_banner;
static lv_obj_t *s_lbl_fido_rp;
static lv_obj_t *s_lbl_fido_user;
static lv_obj_t *s_lbl_fido_action;
static lv_obj_t *s_bar_fido_timeout;
static lv_obj_t *s_lbl_fido_sec;

/* Change passcode widgets */
static lv_obj_t *s_lbl_cp_step;
static lv_obj_t *s_lbl_cp_input;
static lv_obj_t *s_lbl_cp_count;
static lv_obj_t *s_lbl_cp_msg;

/* Set time widgets */
static lv_obj_t *s_lbl_st_title;
static lv_obj_t *s_lbl_st_fields[6];
static lv_obj_t *s_lbl_st_hint;

/* Lab / experiments widgets */
static lv_obj_t *s_lbl_lab_title;
static lv_obj_t *s_lbl_lab_items[AUTH_LAB_ITEM_COUNT];

/* FIDO credential management widgets */
static lv_obj_t *s_lbl_fido_acc_items[AUTH_MAX_FIDO_RPS];
static lv_obj_t *s_lbl_fido_acc_msg;

static lv_color_t color_bg;
static lv_color_t color_card;
static lv_color_t color_cyan;
static lv_color_t color_green;
static lv_color_t color_amber;
static lv_color_t color_red;
static lv_color_t color_text_pri;
static lv_color_t color_text_mut;

void auth_ui_init(void)
{
    color_bg       = lv_color_hex(0x0C1015);
    color_card     = lv_color_hex(0x151B23);
    color_cyan     = lv_color_hex(0x80D6BD);
    color_green    = lv_color_hex(0x80D6BD);
    color_amber    = lv_color_hex(0xEDC776);
    color_red      = lv_color_hex(0xFF9B9B);
    color_text_pri = lv_color_hex(0xE6EDF3);
    color_text_mut = lv_color_hex(0xA2B0C0);

    s_root_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root_screen, color_bg, 0);
    lv_obj_set_style_bg_opa(s_root_screen, LV_OPA_COVER, 0);

    /* --- Top Bar (0..32) --- */
    s_top_bar = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_top_bar, 240, 32);
    lv_obj_align(s_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_top_bar, color_card, 0);
    lv_obj_set_style_bg_opa(s_top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_top_bar, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_top_bar, 1, 0);
    lv_obj_set_style_radius(s_top_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_top_bar, 20, 0);
    lv_obj_set_style_pad_ver(s_top_bar, 6, 0);
    lv_obj_set_style_pad_column(s_top_bar, 6, 0);
    lv_obj_set_flex_flow(s_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_bar, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_top_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_title = lv_label_create(s_top_bar);
    set_label_text_if_changed(s_lbl_title, "");
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_title, color_text_pri, 0);
    lv_obj_set_flex_grow(s_lbl_title, 1);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_CLIP);

    s_lbl_ble_status = lv_label_create(s_top_bar);
    set_label_text_if_changed(s_lbl_ble_status, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_lbl_ble_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_ble_status, color_text_mut, 0);

    s_lbl_wifi_status = lv_label_create(s_top_bar);
    set_label_text_if_changed(s_lbl_wifi_status, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_lbl_wifi_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_status, color_text_mut, 0);

    s_lbl_battery = lv_label_create(s_top_bar);
    set_label_text_if_changed(s_lbl_battery, LV_SYMBOL_BATTERY_EMPTY " --");
    lv_obj_set_style_text_font(s_lbl_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_battery, color_green, 0);

    /* --- Bottom Footer Bar (288..320) --- */
    s_bottom_bar = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_bottom_bar, 240, 32);
    lv_obj_align(s_bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_bottom_bar, color_card, 0);
    lv_obj_set_style_bg_opa(s_bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_bottom_bar, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_bottom_bar, 1, 0);
    lv_obj_set_style_radius(s_bottom_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bottom_bar, 4, 0);

    s_lbl_footer_hint = lv_label_create(s_bottom_bar);
    set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " PIN  " LV_SYMBOL_OK " OK  Hold:CLR");
    lv_obj_set_style_text_font(s_lbl_footer_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_footer_hint, color_text_mut, 0);
    lv_obj_align(s_lbl_footer_hint, LV_ALIGN_CENTER, 0, 0);

    /* ========================================================
       1. Lock Screen Panel
       ======================================================== */
    s_pnl_lock = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_lock, 230, 246);
    lv_obj_align(s_pnl_lock, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_lock, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_lock, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_lock, 1, 0);
    lv_obj_set_style_radius(s_pnl_lock, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_lock, 6, 0);
    lv_obj_remove_flag(s_pnl_lock, LV_OBJ_FLAG_SCROLLABLE);

    /* Live Clock */
    s_lbl_lock_clock = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_clock, "12:00:00");
    lv_obj_set_style_text_font(s_lbl_lock_clock, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_lock_clock, color_cyan, 0);
    lv_obj_align(s_lbl_lock_clock, LV_ALIGN_TOP_MID, 0, 4);

    /* Live Date */
    s_lbl_lock_date = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_date, "2026-11-08 Sun");
    lv_obj_set_style_text_font(s_lbl_lock_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_lock_date, color_text_mut, 0);
    lv_obj_align(s_lbl_lock_date, LV_ALIGN_TOP_MID, 0, 26);

    /* Lock Title */
    s_lbl_lock_title = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_title, "- LOCKED -");
    lv_obj_set_style_text_font(s_lbl_lock_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_lock_title, color_amber, 0);
    lv_obj_align(s_lbl_lock_title, LV_ALIGN_TOP_MID, 0, 44);

    /* PIN Dots (Compact multiline font to prevent 32-bit overlap) */
    s_lbl_lock_input = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_input, "[ _ _ _ _ ]");
    lv_obj_set_style_text_font(s_lbl_lock_input, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_lock_input, color_cyan, 0);
    lv_obj_align(s_lbl_lock_input, LV_ALIGN_TOP_MID, 0, 68);
    lv_label_set_long_mode(s_lbl_lock_input, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_lock_input, 210);
    lv_obj_set_style_text_align(s_lbl_lock_input, LV_TEXT_ALIGN_CENTER, 0);

    /* Bit Count */
    s_lbl_lock_count = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_count, "Bits: 0 / 32");
    lv_obj_set_style_text_font(s_lbl_lock_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_lock_count, color_text_mut, 0);
    lv_obj_align(s_lbl_lock_count, LV_ALIGN_TOP_MID, 0, 108);

    /* Status & Error Message */
    s_lbl_lock_msg = lv_label_create(s_pnl_lock);
    set_label_text_if_changed(s_lbl_lock_msg, "Press [OK] to unlock");
    lv_obj_set_style_text_font(s_lbl_lock_msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_lock_msg, color_text_pri, 0);
    lv_obj_align(s_lbl_lock_msg, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ========================================================
       2. TOTP Main View Panel
       ======================================================== */
    s_pnl_totp = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_totp, 230, 246);
    lv_obj_align(s_pnl_totp, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_totp, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_totp, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_totp, 1, 0);
    lv_obj_set_style_radius(s_pnl_totp, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_totp, 10, 0);
    lv_obj_remove_flag(s_pnl_totp, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_totp_index = lv_label_create(s_pnl_totp);
    set_label_text_if_changed(s_lbl_totp_index, "1 / 4");
    lv_obj_set_style_text_font(s_lbl_totp_index, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_totp_index, color_text_mut, 0);
    lv_obj_align(s_lbl_totp_index, LV_ALIGN_TOP_RIGHT, 0, 4);

    s_lbl_totp_issuer = lv_label_create(s_pnl_totp);
    set_label_text_if_changed(s_lbl_totp_issuer, "GITHUB");
    lv_obj_set_style_text_font(s_lbl_totp_issuer, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_totp_issuer, color_cyan, 0);
    lv_obj_align(s_lbl_totp_issuer, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_width(s_lbl_totp_issuer, 146);
    lv_label_set_long_mode(s_lbl_totp_issuer, LV_LABEL_LONG_DOT);

    s_lbl_totp_account = lv_label_create(s_pnl_totp);
    set_label_text_if_changed(s_lbl_totp_account, "folotoy-dev");
    lv_obj_set_width(s_lbl_totp_account, 206);
    lv_label_set_long_mode(s_lbl_totp_account, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_lbl_totp_account, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_totp_account, color_text_mut, 0);
    lv_obj_align(s_lbl_totp_account, LV_ALIGN_TOP_LEFT, 4, 30);

    /* Large OTP Code */
    s_lbl_totp_code = lv_label_create(s_pnl_totp);
    set_label_text_if_changed(s_lbl_totp_code, "000  000");
    lv_obj_set_style_text_font(s_lbl_totp_code, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_totp_code, color_text_pri, 0);
    lv_obj_align(s_lbl_totp_code, LV_ALIGN_CENTER, 0, -10);

    /* 30s Countdown Bar */
    s_bar_totp_countdown = lv_bar_create(s_pnl_totp);
    lv_obj_set_size(s_bar_totp_countdown, 190, 8);
    lv_obj_align(s_bar_totp_countdown, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_bar_set_range(s_bar_totp_countdown, 0, 30);
    lv_bar_set_value(s_bar_totp_countdown, 30, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_totp_countdown, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_bg_color(s_bar_totp_countdown, color_cyan, LV_PART_INDICATOR);

    s_lbl_totp_seconds = lv_label_create(s_pnl_totp);
    set_label_text_if_changed(s_lbl_totp_seconds, "30s");
    lv_obj_set_style_text_font(s_lbl_totp_seconds, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_totp_seconds, color_cyan, 0);
    lv_obj_align(s_lbl_totp_seconds, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* ========================================================
       3. FIDO2 Prompt Panel
       ======================================================== */
    s_pnl_fido = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_fido, 230, 246);
    lv_obj_align(s_pnl_fido, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_fido, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_fido, color_amber, 0);
    lv_obj_set_style_border_width(s_pnl_fido, 2, 0);
    lv_obj_set_style_radius(s_pnl_fido, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_fido, 10, 0);
    lv_obj_remove_flag(s_pnl_fido, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_fido_banner = lv_label_create(s_pnl_fido);
    set_label_text_if_changed(s_lbl_fido_banner, "! FIDO AUTH REQUEST !");
    lv_obj_set_style_text_font(s_lbl_fido_banner, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fido_banner, color_amber, 0);
    lv_obj_align(s_lbl_fido_banner, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_fido_rp = lv_label_create(s_pnl_fido);
    set_label_text_if_changed(s_lbl_fido_rp, "Site: github.com");
    lv_obj_set_style_text_font(s_lbl_fido_rp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_fido_rp, color_text_pri, 0);
    lv_obj_align(s_lbl_fido_rp, LV_ALIGN_TOP_MID, 0, 38);

    s_lbl_fido_user = lv_label_create(s_pnl_fido);
    set_label_text_if_changed(s_lbl_fido_user, "User: developer");
    lv_obj_set_style_text_font(s_lbl_fido_user, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fido_user, color_text_mut, 0);
    lv_obj_align(s_lbl_fido_user, LV_ALIGN_TOP_MID, 0, 68);

    s_lbl_fido_action = lv_label_create(s_pnl_fido);
    set_label_text_if_changed(s_lbl_fido_action, ">> PRESS [OK] TO APPROVE <<");
    lv_obj_set_style_text_font(s_lbl_fido_action, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fido_action, color_green, 0);
    lv_obj_align(s_lbl_fido_action, LV_ALIGN_CENTER, 0, 20);

    s_bar_fido_timeout = lv_bar_create(s_pnl_fido);
    lv_obj_set_size(s_bar_fido_timeout, 190, 8);
    lv_obj_align(s_bar_fido_timeout, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_bar_set_range(s_bar_fido_timeout, 0, 15);
    lv_bar_set_value(s_bar_fido_timeout, 15, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_fido_timeout, lv_color_hex(0x21262D), 0);
    lv_obj_set_style_bg_color(s_bar_fido_timeout, color_amber, LV_PART_INDICATOR);

    s_lbl_fido_sec = lv_label_create(s_pnl_fido);
    set_label_text_if_changed(s_lbl_fido_sec, "15s timeout");
    lv_obj_set_style_text_font(s_lbl_fido_sec, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fido_sec, color_amber, 0);
    lv_obj_align(s_lbl_fido_sec, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* ========================================================
       5. Change Passcode Panel
       ======================================================== */
    s_pnl_change_pass = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_change_pass, 230, 246);
    lv_obj_align(s_pnl_change_pass, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_change_pass, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_change_pass, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_change_pass, 1, 0);
    lv_obj_set_style_radius(s_pnl_change_pass, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_change_pass, 10, 0);
    lv_obj_remove_flag(s_pnl_change_pass, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_cp_step = lv_label_create(s_pnl_change_pass);
    set_label_text_if_changed(s_lbl_cp_step, "STEP 1: ENTER NEW PIN");
    lv_obj_set_style_text_font(s_lbl_cp_step, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_cp_step, color_amber, 0);
    lv_obj_align(s_lbl_cp_step, LV_ALIGN_TOP_MID, 0, 6);

    s_lbl_cp_input = lv_label_create(s_pnl_change_pass);
    set_label_text_if_changed(s_lbl_cp_input, "[ _ _ _ _ ]");
    lv_obj_set_style_text_font(s_lbl_cp_input, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_cp_input, color_cyan, 0);
    lv_obj_align(s_lbl_cp_input, LV_ALIGN_TOP_MID, 0, 36);
    lv_label_set_long_mode(s_lbl_cp_input, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_cp_input, 210);
    lv_obj_set_style_text_align(s_lbl_cp_input, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_cp_count = lv_label_create(s_pnl_change_pass);
    set_label_text_if_changed(s_lbl_cp_count, "Bits: 0 (Min 4, Max 32)");
    lv_obj_set_style_text_font(s_lbl_cp_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_cp_count, color_text_mut, 0);
    lv_obj_align(s_lbl_cp_count, LV_ALIGN_TOP_MID, 0, 78);

    s_lbl_cp_msg = lv_label_create(s_pnl_change_pass);
    set_label_text_if_changed(s_lbl_cp_msg, "[OK] Next/Save | Long [OK] Cancel");
    lv_obj_set_style_text_font(s_lbl_cp_msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_cp_msg, color_text_pri, 0);
    lv_obj_align(s_lbl_cp_msg, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ========================================================
       6. Set Date & Time Panel
       ======================================================== */
    s_pnl_set_time = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_set_time, 230, 246);
    lv_obj_align(s_pnl_set_time, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_set_time, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_set_time, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_set_time, 1, 0);
    lv_obj_set_style_radius(s_pnl_set_time, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_set_time, 8, 0);
    lv_obj_remove_flag(s_pnl_set_time, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_st_title = lv_label_create(s_pnl_set_time);
    set_label_text_if_changed(s_lbl_st_title, "SET DATE & TIME");
    lv_obj_set_style_text_font(s_lbl_st_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_st_title, color_cyan, 0);
    lv_obj_align(s_lbl_st_title, LV_ALIGN_TOP_MID, 0, 4);

    for (int i = 0; i < 6; i++) {
        s_lbl_st_fields[i] = lv_label_create(s_pnl_set_time);
        lv_obj_set_style_text_font(s_lbl_st_fields[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_st_fields[i], color_text_pri, 0);
        lv_obj_align(s_lbl_st_fields[i], LV_ALIGN_TOP_LEFT, 16, 36 + i * 30);
    }

    s_lbl_st_hint = lv_label_create(s_pnl_set_time);
    set_label_text_if_changed(s_lbl_st_hint, "[OK] Field | [HOLD] Save");
    lv_obj_set_style_text_font(s_lbl_st_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_st_hint, color_amber, 0);
    lv_obj_align(s_lbl_st_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ========================================================
       7. Lab / Experiments Panel
       ======================================================== */
    s_pnl_lab = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_lab, 230, 246);
    lv_obj_align(s_pnl_lab, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_lab, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_lab, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_lab, 1, 0);
    lv_obj_set_style_radius(s_pnl_lab, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_lab, 8, 0);
    lv_obj_remove_flag(s_pnl_lab, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_lab_title = lv_label_create(s_pnl_lab);
    set_label_text_if_changed(s_lbl_lab_title, "LAB / EXPERIMENTS");
    lv_obj_set_style_text_font(s_lbl_lab_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_lab_title, color_amber, 0);
    lv_obj_align(s_lbl_lab_title, LV_ALIGN_TOP_MID, 0, 4);

    for (int i = 0; i < AUTH_LAB_ITEM_COUNT; i++) {
        s_lbl_lab_items[i] = lv_label_create(s_pnl_lab);
        lv_obj_set_width(s_lbl_lab_items[i], 206);
        lv_label_set_long_mode(s_lbl_lab_items[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_lbl_lab_items[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_lab_items[i], color_text_pri, 0);
        lv_obj_align(s_lbl_lab_items[i], LV_ALIGN_TOP_LEFT, 6, 36 + i * 30);
    }

    /* ========================================================
       8. FIDO Credential Management Panel
       ======================================================== */
    s_pnl_fido_acc = lv_obj_create(s_root_screen);
    lv_obj_set_size(s_pnl_fido_acc, 230, 246);
    lv_obj_align(s_pnl_fido_acc, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_pnl_fido_acc, color_card, 0);
    lv_obj_set_style_border_color(s_pnl_fido_acc, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(s_pnl_fido_acc, 1, 0);
    lv_obj_set_style_radius(s_pnl_fido_acc, 8, 0);
    lv_obj_set_style_pad_all(s_pnl_fido_acc, 8, 0);

    lv_obj_t *lbl_fido_acc_title = lv_label_create(s_pnl_fido_acc);
    set_label_text_if_changed(lbl_fido_acc_title, "FIDO CREDENTIALS");
    lv_obj_set_style_text_font(lbl_fido_acc_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_fido_acc_title, color_cyan, 0);
    lv_obj_align(lbl_fido_acc_title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_set_scroll_dir(s_pnl_fido_acc, LV_DIR_VER);

    for (int i = 0; i < AUTH_MAX_FIDO_RPS; i++) {
        s_lbl_fido_acc_items[i] = lv_label_create(s_pnl_fido_acc);
        lv_obj_set_width(s_lbl_fido_acc_items[i], 206);
        lv_label_set_long_mode(s_lbl_fido_acc_items[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_lbl_fido_acc_items[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_fido_acc_items[i], color_text_pri, 0);
        lv_obj_align(s_lbl_fido_acc_items[i], LV_ALIGN_TOP_LEFT, 6, 36 + i * 30);
    }

    s_lbl_fido_acc_msg = lv_label_create(s_pnl_fido_acc);
    set_label_text_if_changed(s_lbl_fido_acc_msg, "[OK] Delete  [HOLD] Back");
    lv_obj_set_style_text_font(s_lbl_fido_acc_msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_fido_acc_msg, color_text_mut, 0);
    lv_obj_align(s_lbl_fido_acc_msg, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Load root screen and set initial view */
    lv_screen_load(s_root_screen);
    auth_ui_set_view(AUTH_VIEW_LOCK);
}

void auth_ui_set_view(auth_view_t view)
{
    s_current_view = view;

    lv_obj_add_flag(s_pnl_lock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_totp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_fido, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_change_pass, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_set_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_lab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pnl_fido_acc, LV_OBJ_FLAG_HIDDEN);

    switch (view) {
    case AUTH_VIEW_LOCK:
        set_label_text_if_changed(s_lbl_title, "");
        lv_obj_clear_flag(s_pnl_lock, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " PIN  " LV_SYMBOL_OK " OK  Hold:CLR");
        break;
    case AUTH_VIEW_TOTP:
        lv_obj_clear_flag(s_pnl_totp, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Next  OK:Set  Hold:Lock");
        break;
    case AUTH_VIEW_FIDO_PROMPT:
        lv_obj_clear_flag(s_pnl_fido, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_OK " Approve    " LV_SYMBOL_CLOSE " Reject");
        set_label_text_if_changed(s_lbl_fido_action, ">> PRESS [OK] TO APPROVE <<");
        lv_obj_set_style_text_color(s_lbl_fido_action, lv_color_hex(0x00FF88), 0);
        break;
    case AUTH_VIEW_SETTINGS:
        lv_obj_clear_flag(s_pnl_lab, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Move  " LV_SYMBOL_OK " Select/Exit");
        break;
    case AUTH_VIEW_CHANGE_PASSCODE:
        lv_obj_clear_flag(s_pnl_change_pass, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP " 1  " LV_SYMBOL_DOWN " 0  " LV_SYMBOL_OK " Next  Hold:Back");
        break;
    case AUTH_VIEW_SET_TIME:
        lv_obj_clear_flag(s_pnl_set_time, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " +/-  " LV_SYMBOL_OK " Field  Hold:Save");
        break;
    case AUTH_VIEW_LAB:
    case AUTH_VIEW_WIFI:
        lv_obj_clear_flag(s_pnl_lab, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Move  " LV_SYMBOL_OK " Select  Hold:Back");
        break;
    case AUTH_VIEW_FIDO_ACCOUNTS:
        lv_obj_clear_flag(s_pnl_fido_acc, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_footer_hint, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Move  " LV_SYMBOL_OK " Delete  Hold:Back");
        break;
    default:
        break;
    }
}

auth_view_t auth_ui_get_view(void)
{
    return s_current_view;
}

void auth_ui_update_top_clock(uint64_t current_unix, int8_t timezone_offset, bool format_24h)
{
    if (!s_lbl_title) return;
    char text[16] = "";
    if (s_current_view != AUTH_VIEW_LOCK) {
        auth_ui_format_top_clock(text, sizeof(text), current_unix, timezone_offset, format_24h);
    }
    lv_obj_set_style_text_font(s_lbl_title, format_24h ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0);
    set_label_text_if_changed(s_lbl_title, text);
}

void auth_ui_update_status_bar(bool ble_conn, int8_t battery_soc, bool is_charging, bool battery_protect)
{
    (void)ble_conn;
    if (!s_lbl_battery) return;
    const char *sym = LV_SYMBOL_BATTERY_EMPTY;
    if (battery_soc >= 80) sym = LV_SYMBOL_BATTERY_FULL;
    else if (battery_soc >= 60) sym = LV_SYMBOL_BATTERY_3;
    else if (battery_soc >= 40) sym = LV_SYMBOL_BATTERY_2;
    else if (battery_soc >= 15) sym = LV_SYMBOL_BATTERY_1;
    char bat_buf[32], status[16];
    auth_ui_format_battery(status, sizeof(status), battery_soc, is_charging, battery_protect);
    snprintf(bat_buf, sizeof(bat_buf), "%s %s", sym, status);
    set_label_text_if_changed(s_lbl_battery, bat_buf);
    lv_obj_set_style_text_color(s_lbl_battery,
        lv_color_hex(auth_ui_battery_color(battery_soc, is_charging, battery_protect)), 0);
}

void auth_ui_update_lock_screen(const char *input_str, uint8_t input_len,
                                const char *status_msg, bool is_lockout, uint32_t lockout_rem_sec,
                                int64_t current_unix, int8_t timezone_offset, bool format_24h)
{
    if (s_current_view != AUTH_VIEW_LOCK) return;

    /* Live Clock and Date */
    int64_t local_sec = current_unix + (int64_t)timezone_offset * 3600LL;
    if (local_sec < 0) local_sec = 0;
    time_t t_local = (time_t)local_sec;
    struct tm tm_buf;
    gmtime_r(&t_local, &tm_buf);

    char time_str[32];
    char date_str[32];
    static const char * const s_wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int wday_idx = (tm_buf.tm_wday >= 0 && tm_buf.tm_wday < 7) ? tm_buf.tm_wday : 0;

    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d %s",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, s_wday[wday_idx]);

    if (format_24h) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        int h12 = tm_buf.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %s",
                 h12, tm_buf.tm_min, tm_buf.tm_sec,
                 tm_buf.tm_hour >= 12 ? "PM" : "AM");
    }

    if (current_unix == 0) {
        snprintf(time_str, sizeof(time_str), "--:--:--");
        snprintf(date_str, sizeof(date_str), "SYNC TIME");
    }
    if (s_lbl_lock_clock) set_label_text_if_changed(s_lbl_lock_clock, time_str);
    if (s_lbl_lock_date) set_label_text_if_changed(s_lbl_lock_date, date_str);

    /* Passcode input */
    if (input_str && input_len > 0) {
        char fmt_buf[80];
        size_t fidx = 0;
        for (uint8_t i = 0; i < input_len && fidx + 3 < sizeof(fmt_buf); i++) {
            if (i == 16) {
                fmt_buf[fidx++] = '\n';
            } else if (i > 0 && (i % 4 == 0)) {
                fmt_buf[fidx++] = ' ';
            }
            char ch = input_str[i * 2];
            fmt_buf[fidx++] = ch;
        }
        fmt_buf[fidx] = '\0';
        set_label_text_if_changed(s_lbl_lock_input, fmt_buf);
    } else {
        set_label_text_if_changed(s_lbl_lock_input, "[ _ _ _ _ ]");
    }

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "Bits: %u / 32", (unsigned int)input_len);
    set_label_text_if_changed(s_lbl_lock_count, count_buf);

    if (is_lockout) {
        char lock_buf[48];
        snprintf(lock_buf, sizeof(lock_buf), "LOCKED OUT! Wait %us", (unsigned int)lockout_rem_sec);
        set_label_text_if_changed(s_lbl_lock_msg, lock_buf);
        lv_obj_set_style_text_color(s_lbl_lock_msg, color_red, 0);
    } else {
        set_label_text_if_changed(s_lbl_lock_msg, status_msg ? status_msg : "Press [OK] to unlock");
        if (status_msg && (strstr(status_msg, "WRONG") || strstr(status_msg, "WIPED") ||
                           strstr(status_msg, "LOCKED") || strstr(status_msg, "ERROR"))) {
            lv_obj_set_style_text_color(s_lbl_lock_msg, color_red, 0);
        } else {
            lv_obj_set_style_text_color(s_lbl_lock_msg, color_text_pri, 0);
        }
    }
}

static void apply_totp_layout(auth_totp_view_t mode)
{
    static auth_totp_view_t previous = AUTH_TOTP_VIEW_CARD;
    if (mode == previous) return;
    previous = mode;
    lv_obj_t *rows[AUTH_TOTP_LIST_MAX] = {
        s_lbl_totp_issuer, s_lbl_totp_index, s_lbl_totp_code, s_lbl_totp_seconds,
    };
    int32_t row_height = mode == AUTH_TOTP_VIEW_LIST3 ? 56 : 40;
    int32_t row_padding = (row_height - 2 * lv_font_montserrat_14.line_height - 2) / 2;
    for (unsigned i = 0; i < AUTH_TOTP_LIST_MAX; i++) {
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(rows[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(rows[i], mode == AUTH_TOTP_VIEW_CARD ? LV_SIZE_CONTENT : 198);
        lv_obj_set_height(rows[i], mode == AUTH_TOTP_VIEW_CARD ? LV_SIZE_CONTENT : row_height);
        lv_obj_set_style_radius(rows[i], mode == AUTH_TOTP_VIEW_CARD ? 0 : 6, 0);
        lv_obj_set_style_border_width(rows[i], mode == AUTH_TOTP_VIEW_CARD ? 0 : 1, 0);
        lv_obj_set_style_pad_hor(rows[i], mode == AUTH_TOTP_VIEW_CARD ? 0 : 6, 0);
        lv_obj_set_style_pad_ver(rows[i], mode == AUTH_TOTP_VIEW_CARD ? 0 : row_padding, 0);
        lv_label_set_long_mode(rows[i], mode == AUTH_TOTP_VIEW_CARD ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(rows[i], &lv_font_montserrat_14, 0);
        if (mode != AUTH_TOTP_VIEW_CARD)
            lv_obj_align(rows[i], LV_ALIGN_TOP_LEFT, 4, 4 + (int)i * (mode == AUTH_TOTP_VIEW_LIST3 ? 60 : 44));
    }
    lv_obj_clear_flag(s_lbl_totp_account, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_lbl_totp_account, LV_SIZE_CONTENT);
    if (mode == AUTH_TOTP_VIEW_CARD) {
        lv_obj_set_width(s_lbl_totp_issuer, 146);
        lv_label_set_long_mode(s_lbl_totp_issuer, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_lbl_totp_issuer, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_lbl_totp_issuer, color_text_pri, 0);
        lv_obj_align(s_lbl_totp_issuer, LV_ALIGN_TOP_LEFT, 4, 4);
        lv_obj_set_style_text_color(s_lbl_totp_index, color_text_mut, 0);
        lv_obj_align(s_lbl_totp_index, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_set_style_text_font(s_lbl_totp_code, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_lbl_totp_code, color_text_pri, 0);
        lv_obj_align(s_lbl_totp_code, LV_ALIGN_CENTER, 0, -10);
        lv_obj_set_style_text_color(s_lbl_totp_seconds, color_cyan, 0);
        lv_obj_align(s_lbl_totp_seconds, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_width(s_lbl_totp_account, 206);
        lv_label_set_long_mode(s_lbl_totp_account, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_lbl_totp_account, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_lbl_totp_account, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(s_lbl_totp_account, color_text_mut, 0);
        lv_obj_align(s_lbl_totp_account, LV_ALIGN_TOP_LEFT, 4, 30);
    } else {
        lv_obj_set_width(s_lbl_totp_account, 198);
        lv_label_set_long_mode(s_lbl_totp_account, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_lbl_totp_account, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(s_lbl_totp_account, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_lbl_totp_account, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

void auth_ui_update_totp_list(const auth_totp_list_item_t *items, uint8_t first_index,
                              uint8_t count, uint8_t total, auth_totp_view_t mode)
{
    if (s_current_view != AUTH_VIEW_TOTP) return;
    if (!count || !total) {
        auth_ui_update_totp(NULL, 0, 0, NULL, 0, 30, false, 0);
        return;
    }
    apply_totp_layout(mode);
    lv_obj_t *rows[AUTH_TOTP_LIST_MAX] = {
        s_lbl_totp_issuer, s_lbl_totp_index, s_lbl_totp_code, s_lbl_totp_seconds,
    };
    bool time_set = items[0].code != NULL;
    for (unsigned i = 0; i < AUTH_TOTP_LIST_MAX; i++) {
        if (i >= count) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        char value[16];
        const char *code = items[i].code;
        if (!code) snprintf(value, sizeof(value), "SYNC TIME");
        else if (strlen(code) >= 6 && strlen(code) <= 8) {
            size_t split = strlen(code) / 2;
            snprintf(value, sizeof(value), "%.*s  %s", (int)split, code, code + split);
        } else snprintf(value, sizeof(value), "%s", code);
        char text[112];
        snprintf(text, sizeof(text), "%u %s / %s", first_index + i + 1,
                 items[i].account->issuer, items[i].account->account);
        size_t head = strlen(text);
        for (size_t n = 0; n < head; n++) if ((unsigned char)text[n] < 32) text[n] = ' ';
        if (code) snprintf(text + head, sizeof(text) - head, "\n%s  %us", value, (unsigned)items[i].remaining);
        else snprintf(text + head, sizeof(text) - head, "\n%s", value);
        set_label_text_if_changed(rows[i], text);
        lv_obj_set_style_text_color(rows[i], code && items[i].remaining <= 5 ? color_red : color_text_pri, 0);
        lv_obj_set_style_border_color(rows[i], lv_color_hex(0x41546B), 0);
    }
    lv_obj_add_flag(s_bar_totp_countdown, LV_OBJ_FLAG_HIDDEN);
    set_label_text_if_changed(s_lbl_footer_hint,
                              LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Page  OK:Set  Hold:Lock");
    if (!time_set) {
        lv_obj_set_style_text_color(s_lbl_totp_account, color_amber, 0);
        set_label_text_if_changed(s_lbl_totp_account, "Sync time via Wi-Fi or Settings");
    } else {
        char page[24];
        snprintf(page, sizeof(page), "%u-%u / %u", first_index + 1, first_index + count, total);
        set_label_text_if_changed(s_lbl_totp_account, page);
        lv_obj_set_style_text_color(s_lbl_totp_account, color_text_mut, 0);
    }
}

void auth_ui_update_totp(const auth_totp_account_t *account,
                         uint8_t cur_idx, uint8_t total_acc,
                         const char *code, uint32_t rem_sec, uint32_t period,
                         bool ble_conn, int8_t battery_soc)
{
    (void)ble_conn;
    (void)battery_soc;

    if (s_current_view != AUTH_VIEW_TOTP) return;
    apply_totp_layout(AUTH_TOTP_VIEW_CARD);

    if (!account || total_acc == 0) {
        lv_obj_set_width(s_lbl_totp_issuer, 198);
        lv_obj_set_style_text_font(s_lbl_totp_issuer, &lv_font_montserrat_14, 0);
        set_label_text_if_changed(s_lbl_totp_issuer, "NO TOTP TOKENS");
        set_label_text_if_changed(s_lbl_totp_account, "Add via Wi-Fi web page");
        lv_obj_add_flag(s_lbl_totp_index, LV_OBJ_FLAG_HIDDEN);
        set_label_text_if_changed(s_lbl_totp_code, "------");
        lv_obj_add_flag(s_bar_totp_countdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_totp_seconds, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_footer_hint) {
            set_label_text_if_changed(s_lbl_footer_hint, "OK:Settings  Hold:Lock");
        }
        return;
    }

    lv_obj_set_width(s_lbl_totp_issuer, 146);
    lv_obj_set_style_text_font(s_lbl_totp_issuer, &lv_font_montserrat_20, 0);
    lv_obj_clear_flag(s_bar_totp_countdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_totp_seconds, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_totp_index, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_footer_hint) {
        set_label_text_if_changed(s_lbl_footer_hint,
                          LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " Next  OK:Set  Hold:Lock");
    }

    set_label_text_if_changed(s_lbl_totp_issuer, account->issuer);
    set_label_text_if_changed(s_lbl_totp_account, account->account);

    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%u / %u", (unsigned int)(cur_idx + 1), (unsigned int)total_acc);
    set_label_text_if_changed(s_lbl_totp_index, idx_buf);

    if (!code) {
        set_label_text_if_changed(s_lbl_totp_code, "SYNC TIME");
        lv_obj_add_flag(s_bar_totp_countdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_totp_seconds, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_lbl_totp_seconds, 198);
        lv_obj_set_style_text_font(s_lbl_totp_seconds, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(s_lbl_totp_seconds, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_lbl_totp_seconds, color_amber, 0);
        set_label_text_if_changed(s_lbl_totp_seconds, "Sync time via Wi-Fi or Settings");
        return;
    }
    lv_obj_set_width(s_lbl_totp_seconds, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_lbl_totp_seconds, &lv_font_montserrat_14, 0);

    if (strlen(code) >= 6 && strlen(code) <= 8) {
        char code_spaced[16];
        size_t split = strlen(code) / 2;
        snprintf(code_spaced, sizeof(code_spaced), "%.*s  %s", (int)split, code, code + split);
        set_label_text_if_changed(s_lbl_totp_code, code_spaced);
    } else if (code && strlen(code) == 5) {
        set_label_text_if_changed(s_lbl_totp_code, code);
    } else {
        set_label_text_if_changed(s_lbl_totp_code, "------");
    }

    /* Bar update */
    lv_bar_set_range(s_bar_totp_countdown, 0, period > 0 ? period : 30);
    lv_bar_set_value(s_bar_totp_countdown, rem_sec, LV_ANIM_OFF);

    if (rem_sec <= 5) {
        lv_obj_set_style_bg_color(s_bar_totp_countdown, color_red, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_lbl_totp_seconds, color_red, 0);
    } else if (rem_sec <= 10) {
        lv_obj_set_style_bg_color(s_bar_totp_countdown, color_amber, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_lbl_totp_seconds, color_amber, 0);
    } else {
        lv_obj_set_style_bg_color(s_bar_totp_countdown, color_cyan, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_lbl_totp_seconds, color_cyan, 0);
    }

    char sec_buf[16];
    snprintf(sec_buf, sizeof(sec_buf), "%us", (unsigned int)rem_sec);
    set_label_text_if_changed(s_lbl_totp_seconds, sec_buf);
}

void auth_ui_update_fido_prompt(const fido_request_t *req, uint32_t rem_sec)
{
    if (s_current_view != AUTH_VIEW_FIDO_PROMPT) return;

    if (req) {
        char rp_buf[80];
        snprintf(rp_buf, sizeof(rp_buf), "Site: %s", req->rp_id);
        set_label_text_if_changed(s_lbl_fido_rp, rp_buf);

        char user_buf[80];
        snprintf(user_buf, sizeof(user_buf), "User: %s", req->user_name[0] ? req->user_name : "(anonymous)");
        set_label_text_if_changed(s_lbl_fido_user, user_buf);

        if (req->type == FIDO_REQ_REGISTRATION) {
            set_label_text_if_changed(s_lbl_fido_banner, "! FIDO REGISTRATION !");
        } else {
            set_label_text_if_changed(s_lbl_fido_banner, "! FIDO AUTH REQUEST !");
        }
    }

    lv_bar_set_range(s_bar_fido_timeout, 0, 15);
    lv_bar_set_value(s_bar_fido_timeout, rem_sec, LV_ANIM_OFF);

    char to_buf[32];
    snprintf(to_buf, sizeof(to_buf), "%us remaining", (unsigned int)rem_sec);
    set_label_text_if_changed(s_lbl_fido_sec, to_buf);
}

void auth_ui_update_fido_result(bool approved)
{
    if (s_current_view != AUTH_VIEW_FIDO_PROMPT) return;

    if (approved) {
        set_label_text_if_changed(s_lbl_fido_action, ">>> APPROVED! <<<");
        lv_obj_set_style_text_color(s_lbl_fido_action, color_green, 0);
    } else {
        set_label_text_if_changed(s_lbl_fido_action, ">>> REJECTED / TIMEOUT <<<");
        lv_obj_set_style_text_color(s_lbl_fido_action, color_red, 0);
    }
}

void auth_ui_update_settings(uint8_t selected_idx, uint8_t brightness,
                             bool sound_enabled, auth_totp_view_t otp_view, uint16_t auto_sleep_sec,
                             bool auto_wipe_enabled, uint8_t passcode_len,
                             bool wifi_ap_active,
                             uint64_t current_epoch, int8_t timezone_offset,
                             bool format_24h, bool battery_protect_80,
                             bool confirm_wipe)
{
    if (s_current_view != AUTH_VIEW_SETTINGS) return;
    char buf[64];
    char sleep_str[16];
    if (!auto_sleep_sec) snprintf(sleep_str, sizeof(sleep_str), "Never");
    else if (auto_sleep_sec < 60) snprintf(sleep_str, sizeof(sleep_str), "%us", (unsigned)auto_sleep_sec);
    else snprintf(sleep_str, sizeof(sleep_str), "%um", (unsigned)(auto_sleep_sec / 60));
    int64_t local_sec = (int64_t)current_epoch + (int64_t)timezone_offset * 3600LL;
    time_t t_local = local_sec < 0 ? 0 : (time_t)local_sec;
    struct tm tm_buf;
    gmtime_r(&t_local, &tm_buf);
    static const char *const groups[] = {
        "DISPLAY & AUDIO", "POWER", "SECURITY", "NETWORK", "TIME", "EXPERIMENTS", "EXIT"
    };
    auth_settings_window_t window = auth_ui_settings_window(selected_idx);
    set_label_text_if_changed(s_lbl_lab_title, "SETTINGS");
    lv_obj_set_style_text_color(s_lbl_lab_title, color_amber, 0);
    for (uint8_t row = 0; row < AUTH_LAB_ITEM_COUNT; row++) {
        lv_obj_clear_flag(s_lbl_lab_items[row], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_lbl_lab_items[row], row == 0 || row == 4 ?
                                   &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_lab_items[row], color_text_mut, 0);
    }
    set_label_text_if_changed(s_lbl_lab_items[0], groups[window.group]);
    snprintf(buf, sizeof(buf), "Group %u/7  %u/%u", window.group + 1,
             selected_idx + 1, AUTH_SETTINGS_ITEM_COUNT);
    set_label_text_if_changed(s_lbl_lab_items[4], buf);
    for (uint8_t slot = 0; slot < 3; slot++) {
        lv_obj_t *row = s_lbl_lab_items[slot + 1];
        if (slot >= window.count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        uint8_t i = window.first + slot;
        const char *mark = selected_idx == i ? ">" : " ";
        lv_color_t color = selected_idx == i ? color_cyan : color_text_pri;
        switch (i) {
        case 0: snprintf(buf, sizeof(buf), "%s Brightness: %u%%", mark, (unsigned)brightness); break;
        case 1: snprintf(buf, sizeof(buf), "%s Audio Beep: %s", mark, sound_enabled ? "ON" : "OFF"); break;
        case 2: snprintf(buf, sizeof(buf), "%s OTP View: %s", mark,
                         otp_view == AUTH_TOTP_VIEW_LIST3 ? "List 3" : otp_view == AUTH_TOTP_VIEW_LIST4 ? "List 4" : "Card"); break;
        case 3: snprintf(buf, sizeof(buf), "%s Bat Protect (80%%): %s", mark, battery_protect_80 ? "ON" : "OFF"); break;
        case 4: snprintf(buf, sizeof(buf), "%s Auto Sleep: %s", mark, sleep_str); break;
        case 5: snprintf(buf, sizeof(buf), "%s [Sleep Device Now]", mark); break;
        case 6: snprintf(buf, sizeof(buf), "%s Auto-Wipe (>8 err): %s", mark, auto_wipe_enabled ? "ON" : "OFF"); break;
        case AUTH_SETTINGS_WIPE_INDEX:
            snprintf(buf, sizeof(buf), "%s %s", mark, confirm_wipe ? "CONFIRM WIPE? Press OK" : "[Wipe All Tokens Now]");
            if (selected_idx == i || confirm_wipe) color = color_red;
            break;
        case 8: snprintf(buf, sizeof(buf), "%s Change PIN (%u bits)", mark, (unsigned)passcode_len); break;
        case 9: snprintf(buf, sizeof(buf), "%s Wi-Fi Hotspot: %s >", mark, wifi_ap_active ? "ON" : "OFF"); break;
        case 10: snprintf(buf, sizeof(buf), "%s Set Date/Time [%02d:%02d]", mark, tm_buf.tm_hour, tm_buf.tm_min); break;
        case 11: snprintf(buf, sizeof(buf), "%s Format: %s", mark, format_24h ? "24-Hour" : "12-Hour"); break;
        case 12: snprintf(buf, sizeof(buf), "%s Timezone: UTC%+d", mark, timezone_offset); break;
        case 13:
            snprintf(buf, sizeof(buf), "%s Lab / Experiments", mark);
            if (selected_idx == i) color = color_amber;
            break;
        case 14: snprintf(buf, sizeof(buf), "%s Exit to TOTP", mark); break;
        }
        set_label_text_if_changed(row, buf);
        lv_obj_set_style_text_color(row, color, 0);
    }
}

void auth_ui_update_change_passcode(uint8_t step, const char *input_str, uint8_t input_len,
                                    const char *msg, bool is_error)
{
    if (s_current_view != AUTH_VIEW_CHANGE_PASSCODE) return;

    if (step == 0) {
        set_label_text_if_changed(s_lbl_cp_step, "STEP 1: ENTER NEW PIN");
    } else {
        set_label_text_if_changed(s_lbl_cp_step, "STEP 2: RE-ENTER TO CONFIRM");
    }

    if (input_str && input_len > 0) {
        char fmt_buf[80];
        size_t fidx = 0;
        for (uint8_t i = 0; i < input_len && fidx + 3 < sizeof(fmt_buf); i++) {
            if (i == 16) {
                fmt_buf[fidx++] = '\n';
            } else if (i > 0 && (i % 4 == 0)) {
                fmt_buf[fidx++] = ' ';
            }
            char ch = input_str[i * 2];
            fmt_buf[fidx++] = ch;
        }
        fmt_buf[fidx] = '\0';
        set_label_text_if_changed(s_lbl_cp_input, fmt_buf);
    } else {
        set_label_text_if_changed(s_lbl_cp_input, "[ _ _ _ _ ]");
    }

    char count_buf[48];
    snprintf(count_buf, sizeof(count_buf), "Bits: %u (Min 4, Max 32)", (unsigned int)input_len);
    set_label_text_if_changed(s_lbl_cp_count, count_buf);

    if (msg) {
        set_label_text_if_changed(s_lbl_cp_msg, msg);
        lv_obj_set_style_text_color(s_lbl_cp_msg, is_error ? color_red : color_text_pri, 0);
    }
}

void auth_ui_update_set_time(uint8_t active_field, int year, int month, int day,
                             int hour, int min, int sec)
{
    if (s_current_view != AUTH_VIEW_SET_TIME) return;

    char buf[48];
    const char *field_names[6] = {"Year", "Month", "Day", "Hour", "Minute", "Second"};
    int vals[6] = {year, month, day, hour, min, sec};

    for (int i = 0; i < 6; i++) {
        bool sel = (i == (int)active_field);
        if (i == 0) {
            snprintf(buf, sizeof(buf), "%s %-6s: [ %04d ]", sel ? ">" : " ", field_names[i], vals[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s %-6s: [  %02d  ]", sel ? ">" : " ", field_names[i], vals[i]);
        }
        set_label_text_if_changed(s_lbl_st_fields[i], buf);
        lv_obj_set_style_text_color(s_lbl_st_fields[i], sel ? color_cyan : color_text_pri, 0);
    }
}

void auth_ui_update_wifi(uint8_t selected_idx, bool active, const char *ssid, const char *password)
{
    if (s_current_view != AUTH_VIEW_WIFI) return;
    set_label_text_if_changed(s_lbl_lab_title, "WI-FI HOTSPOT");
    lv_obj_set_style_text_color(s_lbl_lab_title, color_amber, 0);
    int selected_row = selected_idx == 0 ? 0 : 4;
    for (int i = 0; i < AUTH_LAB_ITEM_COUNT; i++) {
        lv_obj_clear_flag(s_lbl_lab_items[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_lbl_lab_items[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_lab_items[i],
            i == selected_row ? color_cyan : i == 3 ? color_text_mut : color_text_pri, 0);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s Hotspot: %s", selected_idx == 0 ? ">" : " ", active ? "ON" : "OFF");
    set_label_text_if_changed(s_lbl_lab_items[0], buf);
    snprintf(buf, sizeof(buf), "SSID: %s", ssid);
    set_label_text_if_changed(s_lbl_lab_items[1], buf);
    snprintf(buf, sizeof(buf), "Key: %s", password);
    set_label_text_if_changed(s_lbl_lab_items[2], buf);
    set_label_text_if_changed(s_lbl_lab_items[3], "Web: 192.168.4.1");
    snprintf(buf, sizeof(buf), "%s Back", selected_idx == 1 ? ">" : " ");
    set_label_text_if_changed(s_lbl_lab_items[4], buf);
}

void auth_ui_update_lab(uint8_t selected_idx, bool ble_fido_enabled,
                        bool console_log_enabled, bool confirm_fido_reset)
{
    if (s_current_view != AUTH_VIEW_LAB) return;
    set_label_text_if_changed(s_lbl_lab_title, "LAB / EXPERIMENTS");
    lv_obj_set_style_text_color(s_lbl_lab_title, color_amber, 0);
    for (int i = 0; i < AUTH_LAB_ITEM_COUNT; i++) {
        lv_obj_clear_flag(s_lbl_lab_items[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(s_lbl_lab_items[i], &lv_font_montserrat_14, 0);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s BLE FIDO: %s",
             selected_idx == 0 ? ">" : " ", ble_fido_enabled ? "ON" : "OFF");
    set_label_text_if_changed(s_lbl_lab_items[0], buf);
    lv_obj_set_style_text_color(s_lbl_lab_items[0],
        selected_idx == 0 ? color_cyan : (ble_fido_enabled ? color_green : color_text_pri), 0);

    snprintf(buf, sizeof(buf), "%s USB Log Console: %s",
             selected_idx == 1 ? ">" : " ", console_log_enabled ? "ON" : "OFF");
    set_label_text_if_changed(s_lbl_lab_items[1], buf);
    lv_obj_set_style_text_color(s_lbl_lab_items[1], selected_idx == 1 ? color_cyan : color_text_pri, 0);

    snprintf(buf, sizeof(buf), "%s FIDO Credentials",
             selected_idx == 2 ? ">" : " ");
    set_label_text_if_changed(s_lbl_lab_items[2], buf);
    lv_obj_set_style_text_color(s_lbl_lab_items[2], selected_idx == 2 ? color_cyan : color_text_pri, 0);

    if (confirm_fido_reset) {
        snprintf(buf, sizeof(buf), "%s CONFIRM RESET? Press OK",
                 selected_idx == 3 ? ">" : " ");
    } else {
        snprintf(buf, sizeof(buf), "%s Reset FIDO (all)",
                 selected_idx == 3 ? ">" : " ");
    }
    set_label_text_if_changed(s_lbl_lab_items[3], buf);
    lv_obj_set_style_text_color(s_lbl_lab_items[3],
        (selected_idx == 3 || confirm_fido_reset) ? color_red : color_text_pri, 0);

    snprintf(buf, sizeof(buf), "%s Back",
             selected_idx == 4 ? ">" : " ");
    set_label_text_if_changed(s_lbl_lab_items[4], buf);
    lv_obj_set_style_text_color(s_lbl_lab_items[4], selected_idx == 4 ? color_cyan : color_text_pri, 0);
}

void auth_ui_update_fido_accounts(const auth_fido_site_t *rps, uint8_t count,
                                  uint8_t selected_idx, bool confirm_delete)
{
    if (s_current_view != AUTH_VIEW_FIDO_ACCOUNTS) return;

    char buf[AUTH_FIDO_RP_MAX + 24];
    bool legacy = count && selected_idx < count && rps[selected_idx].legacy;
    confirm_delete = confirm_delete && !legacy;
    if (count == 0) {
        set_label_text_if_changed(s_lbl_fido_acc_items[0], "No FIDO credentials");
        lv_obj_set_style_text_color(s_lbl_fido_acc_items[0], color_text_mut, 0);
        for (int i = 1; i < AUTH_MAX_FIDO_RPS; i++) {
            set_label_text_if_changed(s_lbl_fido_acc_items[i], "");
        }
    } else {
        for (int i = 0; i < AUTH_MAX_FIDO_RPS; i++) {
            if (i < count) {
                bool sel = (i == selected_idx);
                snprintf(buf, sizeof(buf), "%s %s%s", sel ? ">" : " ",
                         rps[i].legacy ? "Legacy: " : "", rps[i].rp_id);
                lv_label_long_mode_t mode = sel ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT;
                if (lv_label_get_long_mode(s_lbl_fido_acc_items[i]) != mode) {
                    lv_label_set_long_mode(s_lbl_fido_acc_items[i], mode);
                }
                set_label_text_if_changed(s_lbl_fido_acc_items[i], buf);
                lv_obj_set_style_text_color(s_lbl_fido_acc_items[i],
                    sel ? (confirm_delete ? color_red : color_cyan) : color_text_pri, 0);
            } else {
                set_label_text_if_changed(s_lbl_fido_acc_items[i], "");
            }
        }
    }

    set_label_text_if_changed(s_lbl_fido_acc_msg,
                      legacy ? "Use site to migrate [HOLD] Back" :
                      confirm_delete ? "Press OK again to delete" : "[OK] Delete  [HOLD] Back");
    lv_obj_set_style_text_color(s_lbl_fido_acc_msg, confirm_delete ? color_red : color_text_mut, 0);

    if (count > 0 && selected_idx < count) {
        lv_obj_scroll_to_view(s_lbl_fido_acc_items[selected_idx], LV_ANIM_OFF);
    }
}

void auth_ui_update_wifi_status(bool active)
{
    if (!s_lbl_wifi_status) return;
    set_label_text_if_changed(s_lbl_wifi_status, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_lbl_wifi_status, active ? color_green : color_text_mut, 0);
}

void auth_ui_update_ble_status(bool enabled, bool connected)
{
    if (!s_lbl_ble_status) return;
    set_label_text_if_changed(s_lbl_ble_status, LV_SYMBOL_BLUETOOTH);
    if (!enabled) {
        lv_obj_set_style_text_color(s_lbl_ble_status, color_text_mut, 0);
    } else if (connected) {
        lv_obj_set_style_text_color(s_lbl_ble_status, color_green, 0);
    } else {
        lv_obj_set_style_text_color(s_lbl_ble_status, color_amber, 0);
    }
}

#else

/* Host mock */
static auth_view_t s_mock_view = AUTH_VIEW_LOCK;
void auth_ui_init(void) {}
void auth_ui_set_view(auth_view_t view) { s_mock_view = view; }
auth_view_t auth_ui_get_view(void) { return s_mock_view; }
void auth_ui_update_status_bar(bool ble_conn, int8_t battery_soc, bool is_charging, bool battery_protect) {
    (void)ble_conn; (void)battery_soc; (void)is_charging; (void)battery_protect;
}
void auth_ui_update_lock_screen(const char *i, uint8_t l, const char *m, bool k, uint32_t s,
                                int64_t u, int8_t tz, bool f) {
    (void)i; (void)l; (void)m; (void)k; (void)s; (void)u; (void)tz; (void)f;
}
void auth_ui_update_totp(const auth_totp_account_t *a, uint8_t c, uint8_t t, const char *k, uint32_t r, uint32_t p, bool b, int8_t s) {
    (void)a; (void)c; (void)t; (void)k; (void)r; (void)p; (void)b; (void)s;
}
void auth_ui_update_fido_prompt(const fido_request_t *r, uint32_t s) { (void)r; (void)s; }
void auth_ui_update_fido_result(bool a) { (void)a; }
void auth_ui_update_settings(uint8_t i, uint8_t b, bool s, auth_totp_view_t v, uint16_t a, bool w, uint8_t p,
                              bool wf, uint64_t epoch, int8_t tz, bool fmt, bool bp, bool cw) {
    (void)i;(void)b;(void)s;(void)v;(void)a;(void)w;(void)p;(void)wf;
    (void)epoch;(void)tz;(void)fmt;(void)bp;(void)cw;
}
void auth_ui_update_totp_list(const auth_totp_list_item_t *items, uint8_t first,
                              uint8_t count, uint8_t total, auth_totp_view_t mode) {
    (void)items;(void)first;(void)count;(void)total;(void)mode;
}
void auth_ui_update_top_clock(uint64_t u, int8_t tz, bool f) { (void)u; (void)tz; (void)f; }
void auth_ui_update_wifi(uint8_t i, bool a, const char *s, const char *p) {
    (void)i; (void)a; (void)s; (void)p;
}
void auth_ui_update_change_passcode(uint8_t s, const char *i, uint8_t l, const char *m, bool e) {
    (void)s; (void)i; (void)l; (void)m; (void)e;
}
void auth_ui_update_set_time(uint8_t a, int y, int mo, int d, int h, int mi, int se) {
    (void)a; (void)y; (void)mo; (void)d; (void)h; (void)mi; (void)se;
}
void auth_ui_update_lab(uint8_t s, bool b, bool c, bool cr) {
    (void)s; (void)b; (void)c; (void)cr;
}
void auth_ui_update_fido_accounts(const auth_fido_site_t *rps, uint8_t c,
                                  uint8_t s, bool d) {
    (void)rps; (void)c; (void)s; (void)d;
}
void auth_ui_update_wifi_status(bool a) { (void)a; }
void auth_ui_update_ble_status(bool e, bool c) { (void)e; (void)c; }

#endif
