/* 持久化层：账号 blob 保持版本化迁移，重试次数与视图偏好使用独立小键。
 * 迁移和保存失败必须保留原数据；读取失败不能等同空账号，更不能自动擦除整个 NVS。
 */
#include "auth_storage.h"
#include "totp_crypto.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_rtc_time.h"
#include "rtc_clock.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
static const char *TAG = "auth_storage";
#define STORAGE_NVS_NAMESPACE "fido_auth"
#define STORAGE_NVS_KEY       "config"
#endif

typedef struct {
    char issuer[32];
    char account[32];
    char secret[64];
    uint8_t digits;
    uint8_t period;
} auth_legacy_account_t;

typedef struct {
    uint8_t brightness;
    bool sound_enabled;
    bool auto_wipe_enabled;
    uint32_t passcode_pattern;
    uint8_t passcode_len;
    int64_t unix_time_offset;
    uint16_t auto_sleep_sec;
    int8_t timezone_offset;
    bool time_format_24h;
    bool battery_protect_80;
    uint8_t account_count;
    auth_legacy_account_t accounts[AUTH_MAX_ACCOUNTS];
} auth_legacy_config_t;

static uint32_t s_account_revision = 1;

uint32_t auth_storage_account_revision(void)
{
    return s_account_revision;
}

static void populate_defaults(auth_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness = 80;
    cfg->sound_enabled = true;
    cfg->auto_wipe_enabled = true;
    cfg->passcode_pattern = 3;
    cfg->passcode_len = 4;
    cfg->auto_sleep_sec = 60;
    cfg->timezone_offset = 8;
    cfg->time_format_24h = true;
    cfg->battery_protect_80 = true;
}

static bool valid_account(const auth_totp_account_t *account)
{
    if (!account || !memchr(account->issuer, 0, sizeof(account->issuer)) ||
        !memchr(account->account, 0, sizeof(account->account)) ||
        !memchr(account->secret, 0, sizeof(account->secret)) ||
        !account->issuer[0] || !account->account[0] || !account->secret[0]) return false;
    if (account->algorithm > TOTP_ALGO_STEAM || account->period == 0) return false;
    if (account->algorithm == TOTP_ALGO_STEAM) {
        if (account->digits != 5 || account->period != 30) return false;
    } else if (account->digits < 6 || account->digits > 8) {
        return false;
    }
    uint8_t raw[80];
    int len = totp_base32_decode(account->secret, raw, sizeof(raw));
    volatile uint8_t *p = raw;
    for (size_t i = 0; i < sizeof(raw); i++) p[i] = 0;
    return len > 0;
}

static bool valid_config(const auth_config_t *cfg)
{
    if (!cfg || cfg->account_count > AUTH_MAX_ACCOUNTS || cfg->brightness < 10 ||
        cfg->brightness > 100 || cfg->passcode_len < 4 || cfg->passcode_len > 32 ||
        cfg->timezone_offset < -12 || cfg->timezone_offset > 14) return false;
    for (uint8_t i = 0; i < cfg->account_count; i++) {
        if (!valid_account(&cfg->accounts[i])) return false;
    }
    return true;
}

static bool migrate_legacy(const auth_legacy_config_t *old, auth_config_t *cfg)
{
    if (!old || !cfg || old->account_count > AUTH_MAX_ACCOUNTS) return false;
    _Static_assert(offsetof(auth_config_t, accounts) == offsetof(auth_legacy_config_t, accounts), "legacy header layout changed");
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg, old, offsetof(auth_config_t, accounts));
    for (uint8_t i = 0; i < old->account_count; i++) {
        if (!memchr(old->accounts[i].issuer, 0, 32) || !memchr(old->accounts[i].account, 0, 32) ||
            !memchr(old->accounts[i].secret, 0, 64)) return false;
        memcpy(cfg->accounts[i].issuer, old->accounts[i].issuer, 32);
        memcpy(cfg->accounts[i].account, old->accounts[i].account, 32);
        memcpy(cfg->accounts[i].secret, old->accounts[i].secret, 64);
        cfg->accounts[i].digits = old->accounts[i].digits;
        cfg->accounts[i].period = old->accounts[i].period;
        cfg->accounts[i].algorithm = TOTP_ALGO_SHA1;
    }
    return valid_config(cfg);
}

static int64_t get_monotonic_sec(void)
{
#ifdef ESP_PLATFORM
    return (int64_t)(esp_rtc_get_time_us() / 1000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec;
#endif
}

bool auth_storage_init(auth_config_t *cfg)
{
    if (!cfg) return false;
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(handle, "config_v2", cfg, &len);
    if (err == ESP_OK) {
        nvs_close(handle);
        return len == sizeof(*cfg) && valid_config(cfg);
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return false;
    }
    len = 0;
    err = nvs_get_blob(handle, STORAGE_NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        populate_defaults(cfg);
        return auth_storage_save(cfg);
    }
    if (err != ESP_OK || len != sizeof(auth_legacy_config_t)) {
        nvs_close(handle);
        return false;
    }
    auth_legacy_config_t *old = malloc(sizeof(*old));
    if (!old) {
        nvs_close(handle);
        return false;
    }
    err = nvs_get_blob(handle, STORAGE_NVS_KEY, old, &len);
    bool ok = err == ESP_OK && len == sizeof(*old) && migrate_legacy(old, cfg);
    volatile uint8_t *p = (volatile uint8_t *)old;
    for (size_t i = 0; i < sizeof(*old); i++) p[i] = 0;
    free(old);
    nvs_close(handle);
    if (!ok || !auth_storage_save(cfg)) return false;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_erase_key(handle, STORAGE_NVS_KEY) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Migrated legacy accounts to config_v2");
    return true;
#else
    populate_defaults(cfg);
    return true;
#endif
}

#ifdef ESP_PLATFORM
static bool load_byte(const char *key, uint8_t *value)
{
    *value = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    err = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

static bool save_byte(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
#else
static uint8_t s_host_pin_failures;
static auth_totp_view_t s_host_totp_view;
#endif

bool auth_storage_pin_failures_load(uint8_t *count)
{
    if (!count) return false;
#ifdef ESP_PLATFORM
    return load_byte("pin_fails", count);
#else
    *count = s_host_pin_failures;
    return true;
#endif
}

bool auth_storage_pin_failures_save(uint8_t count)
{
#ifdef ESP_PLATFORM
    return save_byte("pin_fails", count);
#else
    s_host_pin_failures = count;
    return true;
#endif
}

bool auth_storage_totp_view_load(auth_totp_view_t *mode)
{
    if (!mode) return false;
    *mode = AUTH_TOTP_VIEW_CARD;
#ifdef ESP_PLATFORM
    uint8_t value;
    if (!load_byte("totp_view", &value)) return false;
#else
    uint8_t value = (uint8_t)s_host_totp_view;
#endif
    if (value != AUTH_TOTP_VIEW_CARD && value != AUTH_TOTP_VIEW_LIST3 && value != AUTH_TOTP_VIEW_LIST4) return false;
    *mode = (auth_totp_view_t)value;
    return true;
}

bool auth_storage_totp_view_save(auth_totp_view_t mode)
{
    if (mode != AUTH_TOTP_VIEW_CARD && mode != AUTH_TOTP_VIEW_LIST3 && mode != AUTH_TOTP_VIEW_LIST4) return false;
#ifdef ESP_PLATFORM
    return save_byte("totp_view", (uint8_t)mode);
#else
    s_host_totp_view = mode;
    return true;
#endif
}

bool auth_storage_lab_load(auth_lab_config_t *lab)
{
    if (!lab) return false;
    lab->ble_fido_enabled = false;
    lab->console_log_enabled = true;

#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        size_t len = sizeof(*lab);
        esp_err_t err = nvs_get_blob(handle, "lab", lab, &len);
        nvs_close(handle);
        if (err == ESP_OK && len == sizeof(*lab)) {
            return true;
        }
        lab->ble_fido_enabled = false;
        lab->console_log_enabled = true;
    }
#endif
    return true;
}

bool auth_storage_lab_save(const auth_lab_config_t *lab)
{
    if (!lab) return false;

#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "lab", lab, sizeof(*lab));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
#else
    return true;
#endif
}

bool auth_storage_save(const auth_config_t *cfg)
{
    if (!valid_config(cfg)) return false;
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "config_v2", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
#else
    return true;
#endif
}

bool auth_storage_add_account_ex(auth_config_t *cfg, const auth_totp_account_t *account)
{
    if (!valid_config(cfg) || !valid_account(account) || cfg->account_count >= AUTH_MAX_ACCOUNTS) return false;
    uint8_t index = cfg->account_count;
    cfg->accounts[index] = *account;
    cfg->account_count++;
    if (auth_storage_save(cfg)) {
        ++s_account_revision;
        return true;
    }
    cfg->account_count--;
    memset(&cfg->accounts[index], 0, sizeof(cfg->accounts[index]));
    return false;
}

bool auth_storage_add_account(auth_config_t *cfg, const char *issuer, const char *account, const char *secret)
{
    if (!issuer || !account || !secret || strlen(issuer) >= AUTH_ISSUER_MAX_LEN ||
        strlen(account) >= AUTH_ACCOUNT_MAX_LEN || strlen(secret) >= AUTH_SECRET_MAX_LEN) return false;
    auth_totp_account_t value = {.digits = 6, .period = 30, .algorithm = TOTP_ALGO_SHA1};
    strcpy(value.issuer, issuer);
    strcpy(value.account, account);
    strcpy(value.secret, secret);
    bool ok = auth_storage_add_account_ex(cfg, &value);
    volatile uint8_t *p = (volatile uint8_t *)&value;
    for (size_t i = 0; i < sizeof(value); i++) p[i] = 0;
    return ok;
}

bool auth_storage_update_account(auth_config_t *cfg, uint8_t index, const auth_totp_account_t *account)
{
    if (!valid_config(cfg) || index >= cfg->account_count || !valid_account(account)) return false;
    auth_totp_account_t old = cfg->accounts[index];
    cfg->accounts[index] = *account;
    bool ok = auth_storage_save(cfg);
    if (ok) ++s_account_revision;
    if (!ok) cfg->accounts[index] = old;
    volatile uint8_t *p = (volatile uint8_t *)&old;
    for (size_t i = 0; i < sizeof(old); i++) p[i] = 0;
    return ok;
}

bool auth_storage_remove_account(auth_config_t *cfg, uint8_t index)
{
    if (!valid_config(cfg) || index >= cfg->account_count) return false;
    auth_totp_account_t old = cfg->accounts[index];
    size_t count = cfg->account_count - index - 1;
    memmove(&cfg->accounts[index], &cfg->accounts[index + 1], count * sizeof(old));
    cfg->account_count--;
    memset(&cfg->accounts[cfg->account_count], 0, sizeof(old));
    bool ok = auth_storage_save(cfg);
    if (ok) ++s_account_revision;
    if (!ok) {
        memmove(&cfg->accounts[index + 1], &cfg->accounts[index], count * sizeof(old));
        cfg->accounts[index] = old;
        cfg->account_count++;
    }
    volatile uint8_t *p = (volatile uint8_t *)&old;
    for (size_t i = 0; i < sizeof(old); i++) p[i] = 0;
    return ok;
}

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include <sys/time.h>
static RTC_DATA_ATTR bool s_time_set;
#else
static bool s_time_set;
#endif

void auth_time_init(bool deep_sleep_wakeup)
{
    if (!deep_sleep_wakeup) s_time_set = false;
}

bool auth_time_is_set(void)
{
    return s_time_set;
}

uint64_t auth_get_current_timestamp(const auth_config_t *cfg)
{
    if (!cfg || !s_time_set) return 0;
#ifdef ESP_PLATFORM
    struct timeval now;
    gettimeofday(&now, NULL);
    return (uint64_t)now.tv_sec;
#else
    int64_t total = cfg->unix_time_offset + get_monotonic_sec();
    return total > 0 ? (uint64_t)total : 0;
#endif
}

bool auth_set_current_timestamp(auth_config_t *cfg, uint64_t unix_sec)
{
    if (!cfg || unix_sec < 1577836800ULL || unix_sec > 4102444800ULL) return false;
#ifdef ESP_PLATFORM
    struct timeval now = {.tv_sec = (time_t)unix_sec, .tv_usec = 0};
    // 系统拒绝校时后，不能更新偏移或把未校时状态标为成功。
    if (settimeofday(&now, NULL) != 0) return false;
#endif
    rtc_clock_wall_time_set();
    cfg->unix_time_offset = (int64_t)unix_sec - get_monotonic_sec();
    s_time_set = true;
    return true;
}

bool auth_set_reference_timestamp(auth_config_t *cfg, const rtc_clock_reference_t *reference)
{
    if (!cfg || !reference) return false;
    const rtc_clock_reference_result_t result = rtc_clock_apply_reference(reference);
    if (result == RTC_CLOCK_REFERENCE_FAILED) return false;
    if (result == RTC_CLOCK_REFERENCE_UNCERTAIN) {
        s_time_set = false;
        return false;
    }
#ifdef ESP_PLATFORM
    const int64_t hrt = esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t hrt = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
    const uint64_t age = hrt >= reference->hrt_us ? (uint64_t)(hrt - reference->hrt_us) : 0;
    cfg->unix_time_offset = (int64_t)((reference->utc_us + age) / 1000000) - get_monotonic_sec();
    s_time_set = true;
    return true;
}

bool auth_storage_wipe_all(auth_config_t *cfg)
{
    if (!valid_config(cfg)) return false;
    auth_config_t *next = malloc(sizeof(*next));
    if (!next) return false;
    *next = *cfg;
    next->account_count = 0;
    memset(next->accounts, 0, sizeof(next->accounts));
    bool ok = auth_storage_save(next);
    if (ok) {
        *cfg = *next;
        ++s_account_revision;
    }
    volatile uint8_t *p = (volatile uint8_t *)next;
    for (size_t i = 0; i < sizeof(*next); i++) p[i] = 0;
    free(next);
    return ok;
}

#ifdef ESP_PLATFORM
#define FIDO_NVS_NAMESPACE "fido_sec"
#define FIDO_PIN_NAMESPACE "fido_pin"
#define FIDO_NVS_PARTITION "nvs"

uint8_t auth_storage_fido_list(char rps[][AUTH_FIDO_RP_KEY_MAX], uint8_t max_rps)
{
    if (!rps || max_rps == 0) return 0;

    nvs_handle_t handle;
    if (nvs_open(FIDO_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }

    uint8_t count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(FIDO_NVS_PARTITION, FIDO_NVS_NAMESPACE,
                                   NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it && count < max_rps) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "u_", 2) == 0 && info.key[2] != '\0') {
            snprintf(rps[count], AUTH_FIDO_RP_KEY_MAX, "%.12s", info.key + 2);
            count++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);
    return count;
}

bool auth_storage_fido_delete(const char *rp_key)
{
    if (!rp_key || rp_key[0] == '\0') return false;

    nvs_handle_t handle;
    if (nvs_open(FIDO_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    char key[16];
    snprintf(key, sizeof(key), "u_%.12s", rp_key);
    nvs_erase_key(handle, key);
    snprintf(key, sizeof(key), "s_%.12s", rp_key);
    nvs_erase_key(handle, key);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool auth_storage_fido_reset_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FIDO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }

    nvs_handle_t pin_handle;
    esp_err_t pin_err = nvs_open(FIDO_PIN_NAMESPACE, NVS_READWRITE, &pin_handle);
    if (pin_err == ESP_OK) {
        pin_err = nvs_erase_all(pin_handle);
        if (pin_err == ESP_OK) pin_err = nvs_commit(pin_handle);
        nvs_close(pin_handle);
    }
    return err == ESP_OK && pin_err == ESP_OK;
}

#else

uint8_t auth_storage_fido_list(char rps[][AUTH_FIDO_RP_KEY_MAX], uint8_t max_rps)
{
    (void)rps;
    (void)max_rps;
    return 0;
}

bool auth_storage_fido_delete(const char *rp_key)
{
    (void)rp_key;
    return false;
}

bool auth_storage_fido_reset_all(void)
{
    return false;
}

#endif
