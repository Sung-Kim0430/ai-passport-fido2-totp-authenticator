#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/rtc_clock.h"

static rtc_clock_reference_result_t reference_result;
static unsigned reference_calls, manual_calls;
rtc_clock_reference_result_t rtc_clock_apply_reference(const rtc_clock_reference_t *ref) {
    assert(ref);
    ++reference_calls;
    return reference_result;
}
void rtc_clock_wall_time_set(void) { ++manual_calls; }

#include "../main/auth_storage.c"

int main(void)
{
    auth_config_t cfg;
    assert(auth_storage_init(&cfg));
    assert(cfg.account_count == 0);
    auth_legacy_config_t legacy = {0};
    legacy.brightness = 80;
    legacy.passcode_len = 4;
    legacy.passcode_pattern = 3;
    legacy.auto_sleep_sec = 60;
    legacy.timezone_offset = 8;
    legacy.account_count = 1;
    strcpy(legacy.accounts[0].issuer, "Example");
    strcpy(legacy.accounts[0].account, "test");
    strcpy(legacy.accounts[0].secret, "JBSWY3DPEHPK3PXP");
    legacy.accounts[0].digits = 8;
    legacy.accounts[0].period = 60;
    assert(migrate_legacy(&legacy, &cfg));
    assert(cfg.account_count == 1);
    assert(cfg.accounts[0].algorithm == TOTP_ALGO_SHA1);
    assert(cfg.accounts[0].digits == 8 && cfg.accounts[0].period == 60);
    assert(strcmp(cfg.accounts[0].secret, legacy.accounts[0].secret) == 0);
    auth_totp_account_t account = cfg.accounts[0];
    account.algorithm = TOTP_ALGO_SHA512;
    uint32_t revision = auth_storage_account_revision();
    assert(auth_storage_add_account_ex(&cfg, &account));
    assert(auth_storage_account_revision() == revision + 1);
    assert(cfg.account_count == 2);
    account.algorithm = TOTP_ALGO_STEAM;
    account.digits = 5;
    account.period = 30;
    assert(auth_storage_update_account(&cfg, 1, &account));
    assert(cfg.accounts[1].algorithm == TOTP_ALGO_STEAM);
    account.digits = 8;
    assert(!auth_storage_add_account_ex(&cfg, &account));
    account.digits = 5;
    strcpy(account.secret, "invalid!");
    assert(!auth_storage_add_account_ex(&cfg, &account));
    assert(auth_storage_remove_account(&cfg, 0));
    assert(cfg.account_count == 1 && cfg.accounts[0].algorithm == TOTP_ALGO_STEAM);
    assert(auth_storage_wipe_all(&cfg));
    assert(cfg.account_count == 0);
    legacy.account_count = AUTH_MAX_ACCOUNTS + 1;
    assert(!migrate_legacy(&legacy, &cfg));
    auth_time_init(false);
    assert(!auth_time_is_set());
    assert(auth_get_current_timestamp(&cfg) == 0);
    assert(!auth_set_current_timestamp(&cfg, 0));
    assert(auth_set_current_timestamp(&cfg, 1772983200ULL));
    assert(auth_time_is_set());
    assert(manual_calls == 1);
    uint64_t now = auth_get_current_timestamp(&cfg);
    assert(now >= 1772983200ULL && now <= 1772983201ULL);
    auth_time_init(true);
    assert(auth_time_is_set());
    struct timespec reference_hrt;
    assert(clock_gettime(CLOCK_MONOTONIC, &reference_hrt) == 0);
    rtc_clock_reference_t ref = {
        .id = 9, .utc_us = UINT64_C(1772983210) * 1000000,
        .hrt_us = (int64_t)reference_hrt.tv_sec * 1000000 + reference_hrt.tv_nsec / 1000,
        .uncertainty_us = 1000, .quality = true
    };
    const int64_t old_offset = cfg.unix_time_offset;
    reference_result = RTC_CLOCK_REFERENCE_FAILED;
    assert(!auth_set_reference_timestamp(&cfg, &ref));
    assert(auth_time_is_set() && cfg.unix_time_offset == old_offset);
    reference_result = RTC_CLOCK_REFERENCE_UNCERTAIN;
    assert(!auth_set_reference_timestamp(&cfg, &ref));
    assert(!auth_time_is_set() && cfg.unix_time_offset == old_offset);
    reference_result = RTC_CLOCK_REFERENCE_FAILED;
    assert(!auth_set_reference_timestamp(&cfg, &ref) && !auth_time_is_set());
    reference_result = RTC_CLOCK_REFERENCE_SET;
    assert(auth_set_reference_timestamp(&cfg, &ref) && auth_time_is_set());
    assert(auth_get_current_timestamp(&cfg) >= 1772983210ULL);
    assert(reference_calls == 4 && manual_calls == 1);
    assert(!auth_set_reference_timestamp(NULL, &ref));
    assert(!auth_set_reference_timestamp(&cfg, NULL));
    assert(reference_calls == 4);
    assert(!auth_set_current_timestamp(&cfg, 4102444801ULL));
    auth_time_init(false);
    assert(!auth_time_is_set());
    assert(auth_get_current_timestamp(&cfg) == 0);
    uint8_t failures = 99;
    assert(auth_storage_pin_failures_load(&failures) && failures == 0);
    assert(auth_storage_pin_failures_save(4));
    assert(auth_storage_init(&cfg));
    assert(auth_storage_pin_failures_load(&failures) && failures == 4);
    assert(auth_storage_pin_failures_save(9));
    assert(auth_storage_pin_failures_load(&failures) && failures == 9);
    assert(auth_storage_pin_failures_save(0));
    assert(auth_storage_pin_failures_load(&failures) && failures == 0);
    auth_totp_view_t mode = AUTH_TOTP_VIEW_LIST4;
    assert(auth_storage_totp_view_load(&mode) && mode == AUTH_TOTP_VIEW_CARD);
    assert(auth_storage_totp_view_save(AUTH_TOTP_VIEW_LIST3));
    assert(auth_storage_init(&cfg));
    assert(auth_storage_totp_view_load(&mode) && mode == AUTH_TOTP_VIEW_LIST3);
    assert(!auth_storage_totp_view_save((auth_totp_view_t)2));
    assert(auth_storage_totp_view_load(&mode) && mode == AUTH_TOTP_VIEW_LIST3);
    assert(auth_storage_totp_view_save(AUTH_TOTP_VIEW_LIST4));
    assert(auth_storage_totp_view_load(&mode) && mode == AUTH_TOTP_VIEW_LIST4);
    assert(auth_storage_totp_view_save(AUTH_TOTP_VIEW_CARD));
    puts("Account validation, migration, clock and independent preferences: PASS");
    return 0;
}
