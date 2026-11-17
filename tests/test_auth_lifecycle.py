import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/main.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"(?:static )?(?:void|bool) " + name + r"\([^)]*\)\n\{[\s\S]*?\n\}", SOURCE)
    if not match:
        raise AssertionError(f"Missing function {name}")
    return match.group()


class ApplicationLifecycleTests(unittest.TestCase):
    def test_worker_tasks_do_not_return(self):
        for name in ("auth_app_task", "auth_tick_task"):
            self.assertNotIn("return;", function(name))

    def test_confirmation_does_not_discard_other_commands(self):
        self.assertNotIn("app_event_t discard", function("auth_app_task"))

    def test_tick_does_not_read_application_owned_state(self):
        tick = function("auth_tick_task")
        self.assertNotIn("s_cfg", tick)
        self.assertNotIn("auth_ui_get_view", tick)

    def test_credential_reset_quiesces_ble(self):
        reset = function("erase_credentials")
        self.assertLess(reset.index("auth_ble_stop()"), reset.index("auth_storage_fido_reset_all()"))
        app = function("auth_app_task")
        self.assertEqual(app.count("erase_credentials(true)"), 1)
        self.assertEqual(app.count("erase_credentials(false)"), 1)
        self.assertEqual(app.count("finish_automatic_wipe()"), 3)
        wipe = function("finish_automatic_wipe")
        self.assertLess(wipe.index("erase_credentials(true)"), wipe.index("auth_storage_pin_failures_save(0)"))

    def test_reboot_loads_persistent_pin_failures(self):
        boot = function("app_main")
        self.assertIn("auth_storage_pin_failures_load", boot)
        self.assertIn("passcode_guard_restore_attempts", boot)
        app = function("auth_app_task")
        self.assertIn("s_guard.failed_attempts != previous_failures", app)
        self.assertIn("auth_storage_pin_failures_save(s_guard.failed_attempts)", app)

    def test_wipe_uses_named_setting_index(self):
        self.assertIn("s_settings_idx = AUTH_SETTINGS_WIPE_INDEX", function("auth_app_task"))

    def test_lockout_uses_rtc_without_requiring_utc(self):
        app = function("auth_app_task")
        self.assertIn("s_rtc_lockout_end_us = (int64_t)esp_rtc_get_time_us()", app)
        self.assertNotIn("now_ep", app)
        self.assertNotIn("auth_get_current_timestamp(&s_cfg)", function("app_main"))

    def test_wifi_retains_ownership_during_failed_cleanup(self):
        wifi = (ROOT / "main/auth_wifi.c").read_text(encoding="utf-8")
        body = re.search(r"bool auth_wifi_ap_is_active\(void\)\n\{[\s\S]*?\n\}", wifi).group()
        self.assertIn("s_wifi_initialized", body)
        self.assertIn("s_ap_netif", body)

    def test_wire_reset_cannot_erase_without_confirmation(self):
        ble = (ROOT / "main/auth_ble.c").read_text(encoding="utf-8")
        branch = re.search(r"if \(ctap_cmd == 0x07\) \{[\s\S]*?\n        \}", ble).group()
        self.assertNotIn("fido_pin_factory_reset", branch)
        self.assertNotIn("FIDO_CMD_RESET", branch)

    def test_totp_buttons_use_view_aware_navigation(self):
        app = function("auth_app_task")
        self.assertIn("auth_ui_totp_navigate(s_cfg.account_count, s_cur_acc_idx, s_totp_view, false)", app)
        self.assertIn("auth_ui_totp_navigate(s_cfg.account_count, s_cur_acc_idx, s_totp_view, true)", app)

    def test_audio_runtime_power_is_explicit(self):
        tone = function("play_tone")
        self.assertIn("bsp_audio_wake()", tone)
        self.assertLess(tone.index("bsp_audio_wake()"), tone.index("bsp_audio_write("))
        self.assertIn("s_audio_idle_at_ms", tone)
        self.assertIn("bsp_audio_sleep()", function("audio_off_now"))
        self.assertIn("bsp_audio_standby()", function("audio_standby_now"))
        idle = function("audio_idle")
        self.assertIn("audio_standby_now()", idle)
        self.assertIn("audio_off_now()", idle)
        self.assertIn("audio_idle(now_ms)", function("auth_app_task"))
        boot = function("app_main")
        self.assertIn("audio_standby_now()", boot)
        self.assertIn("audio_off_now()", boot)
        self.assertIn("if (ok && !s_cfg.sound_enabled) audio_off_now();", SOURCE)

    def test_audio_reminder_runs_outside_lvgl_lock(self):
        refresh = function("refresh_display")
        self.assertLess(refresh.index("sound_hint();"), refresh.index("ui_lock();"))


if __name__ == "__main__":
    unittest.main()
