#!/usr/bin/env python3
"""运行真实校时函数体，并检查时钟任务与终端休眠的接线约束。"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_key_wake_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


class ClockSynchronizationTest(unittest.TestCase):
    def test_failed_settimeofday_does_not_publish_success(self):
        body = function_body((ROOT / "main/auth_storage.c").read_text(),
                             "auth_set_current_timestamp")
        source = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define ESP_PLATFORM 1
typedef int64_t time_t;
struct timeval { time_t tv_sec; int64_t tv_usec; };
typedef struct { int64_t unix_time_offset; } auth_config_t;
static bool s_time_set;
static int fail_set;
static unsigned sets, syncs;
void rtc_clock_wall_time_set(void) { ++syncs; }
static struct timeval applied;
static int64_t get_monotonic_sec(void) { return 7; }
static int settimeofday(const struct timeval *now, void *unused) {
    (void)unused;
    ++sets;
    if (fail_set) return -1;
    applied = *now;
    return 0;
}
bool auth_set_current_timestamp(auth_config_t *cfg, uint64_t unix_sec) {
''' + body + r'''
}
int main(void) {
    auth_config_t cfg = {.unix_time_offset = 123};
    fail_set = 1;
    if (auth_set_current_timestamp(&cfg, 1700000000) ||
        cfg.unix_time_offset != 123 || s_time_set || sets != 1 || syncs != 0) return 1;
    fail_set = 0;
    if (!auth_set_current_timestamp(&cfg, 1700000000) ||
        cfg.unix_time_offset != 1699999993 || !s_time_set ||
        applied.tv_sec != 1700000000 || applied.tv_usec != 0 || sets != 2 || syncs != 1) return 2;
    fail_set = 1;
    if (auth_set_current_timestamp(&cfg, 1700000100) ||
        cfg.unix_time_offset != 1699999993 || !s_time_set || sets != 3 || syncs != 1) return 3;
    if (auth_set_current_timestamp(NULL, 1700000000) ||
        auth_set_current_timestamp(&cfg, 0) ||
        auth_set_current_timestamp(&cfg, 4102444801ULL) || sets != 3 || syncs != 1) return 4;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rtc-sync-", dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            test = Path(directory) / "sync.c"
            output = Path(directory) / "sync"
            test.write_text(source, encoding="utf-8")
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(output)], check=True)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normal_battery_poll_updates_clock_cache(self):
        body = function_body((ROOT / "main/main.c").read_text(), "battery_environment_for_frame")
        self.assertIn("battery_environment_for_frame(now_ms)", function_body((ROOT / "main/main.c").read_text(), "refresh_display"))
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
static unsigned updates;
static int mv = 3900;
int bsp_battery_mv(void) { return mv; }
int64_t esp_timer_get_time(void) { return 7000000; }
void rtc_clock_battery_cache_update(bool valid, uint16_t value, int64_t stamp) {
    ++updates;
    assert(valid == (mv >= 2000 && mv <= 6000));
    assert(value == (valid ? mv : 0));
    assert(stamp == 7000000);
}
static void battery_environment_for_frame(uint64_t now_ms) {
''' + body + r'''
}
int main(void) {
    battery_environment_for_frame(0);
    assert(updates == 1);
    battery_environment_for_frame(100);
    assert(updates == 1);
    mv = -1;
    battery_environment_for_frame(5000);
    assert(updates == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rtc-battery-", dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            test = Path(directory) / "battery.c"
            output = Path(directory) / "battery"
            test.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)

    def test_restore_precedes_slow_board_initialization(self):
        body = function_body((ROOT / "main/main.c").read_text(), "app_main")
        self.assertIn("rtc_clock_boot(", body)
        self.assertLess(body.index("auth_time_init("), body.index("rtc_clock_boot("))
        self.assertLess(body.index("rtc_clock_boot("), body.index("bsp_i2c_init("))
        self.assertIn("RTC_CLOCK_NEEDS_SYNC", body)

    def test_sampling_uses_application_owner_not_keepalive_task(self):
        source = (ROOT / "main/main.c").read_text()
        app = function_body(source, "auth_app_task")
        keepalive = function_body(source, "auth_tick_task")
        self.assertIn("rtc_clock_tick()", app)
        self.assertNotIn("rtc_clock_tick", keepalive)
        self.assertLess(app.index("rtc_clock_tick()"), app.index("ui_lock()"))

    def test_disabled_maintenance_restores_default_stub(self):
        body = function_body((ROOT / "main/rtc_clock_platform.c").read_text(), "rtc_clock_platform_arm")
        self.assertIn("esp_set_deep_sleep_wake_stub(esp_wake_deep_sleep)", body)

    def test_stub_uses_checkpoint_sampled_before_deciding_key_check(self):
        stub = (ROOT / "main/rtc_wake_stub.c").read_text(encoding="utf-8")
        body = function_body(stub, "rtc_clock_wake_stub")
        self.assertIn("const uint64_t commit_tick", body)
        capture = body.index("const uint64_t commit_tick")
        deciding_check = body.index("if (!stub_poll(&watch))", capture)
        commit = body.index("rtc_drift_ledger_commit(", deciding_check)
        self.assertLess(capture, deciding_check)
        self.assertLess(deciding_check, commit)
        self.assertIn("&session->ledger, commit_tick, period", body)

    def test_fast_clock_defaults_and_early_maintenance(self):
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_RTC_CLK_SRC_INT_8MD256=y", defaults)
        self.assertIn("CONFIG_RTC_CLK_CAL_CYCLES=3000", defaults)
        self.assertIn("CONFIG_AUTH_RTC_MAINTENANCE=y", defaults)
        body = function_body((ROOT / "main/main.c").read_text(), "app_main")
        self.assertIn("esp_sleep_get_wakeup_causes()", body)
        self.assertIn("clock_maintenance_try(", body)
        self.assertLess(body.index("clock_key_observer_begin("), body.index("nvs_flash_init("))
        self.assertLess(body.index("rtc_clock_boot("), body.index("clock_maintenance_try("))
        self.assertLess(body.index("clock_maintenance_try("), body.index("bsp_i2c_init("))
        self.assertLess(body.index("clock_key_observer_end("), body.index("bsp_i2c_init("))
        self.assertLess(body.index("key_guard_arm("), body.index("clock_key_observer_begin("))

    def test_actual_maintenance_router_handles_keys_and_rejected_sleep(self):
        body = function_body((ROOT / "main/main.c").read_text(), "clock_maintenance_try")
        for forbidden in ("bsp_display", "bsp_audio", "auth_ble", "auth_wifi", "nvs_", "lv_"):
            self.assertNotIn(forbidden, body)
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
enum { BSP_BTN_GPIO = 0, ESP_GPIO_WAKEUP_GPIO_LOW = 0, ESP_OK = 0 };
static bool due, key_seen, prepared, committed, trusted, complete_sleep, key_during_prepare, key_during_commit;
static int gpio_error, prepares, commits, sleeps, aborts;
static jmp_buf slept;
static bool rtc_clock_maintenance_due(bool timer_only) { return timer_only && due; }
static bool clock_key_pending(void) { return key_seen; }
static bool rtc_clock_prepare_maintenance(void) { ++prepares; if (key_during_prepare) key_seen = true; return prepared; }
static int esp_deep_sleep_enable_gpio_wakeup(uint64_t pins, int level) {
    assert(pins == (UINT64_C(1) << BSP_BTN_GPIO) && level == ESP_GPIO_WAKEUP_GPIO_LOW); return gpio_error;
}
static bool auth_time_is_set(void) { return trusted; }
static bool rtc_clock_commit_sleep(bool valid) { assert(valid == trusted); ++commits; if (key_during_commit) key_seen = true; return committed; }
static int esp_deep_sleep_try_to_start(void) { ++sleeps; if (complete_sleep) longjmp(slept, 1); return -1; }
static void rtc_clock_abort_sleep(void) { ++aborts; }
static void clock_maintenance_try(bool timer_only, bool observer_ready) {
''' + body + r'''
}
static void reset(void) {
    due = prepared = committed = trusted = true;
    key_seen = complete_sleep = key_during_prepare = key_during_commit = false;
    gpio_error = prepares = commits = sleeps = aborts = 0;
}
int main(void) {
    reset(); clock_maintenance_try(false, true); assert(!prepares && !sleeps && !aborts);
    reset(); clock_maintenance_try(true, false); assert(!prepares && !sleeps && !aborts);
    reset(); due = false; clock_maintenance_try(true, true); assert(!prepares && !sleeps && !aborts);
    reset(); key_seen = true; clock_maintenance_try(true, true); assert(!prepares && !sleeps && !aborts);
    reset(); key_during_prepare = true; clock_maintenance_try(true, true); assert(prepares == 1 && !commits && !sleeps && aborts == 1);
    reset(); prepared = false; clock_maintenance_try(true, true); assert(prepares == 1 && !commits && !sleeps && aborts == 1);
    reset(); gpio_error = -1; clock_maintenance_try(true, true); assert(!commits && !sleeps && aborts == 1);
    reset(); committed = false; clock_maintenance_try(true, true); assert(commits == 1 && !sleeps && aborts == 1);
    reset(); key_during_commit = true; clock_maintenance_try(true, true); assert(commits == 1 && !sleeps && aborts == 1);
    reset(); clock_maintenance_try(true, true); assert(sleeps == 1 && aborts == 1);
    reset(); complete_sleep = true;
    if (!setjmp(slept)) { clock_maintenance_try(true, true); assert(false); }
    assert(sleeps == 1 && !aborts);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rtc-maintenance-", dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            test = Path(directory) / "maintenance.c"
            output = Path(directory) / "maintenance"
            test.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)

    def test_actual_key_observer_preserves_released_edges_and_ownership(self):
        main = (ROOT / "main/main.c").read_text()
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { BSP_BTN_GPIO = 0, ESP_OK = 0, GPIO_STATUS_REG = 1, GPIO_MODE_INPUT = 1,
       GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0,
       GPIO_INTR_NEGEDGE = 2, ESP_INTR_FLAG_IRAM = 4 };
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static volatile bool s_clock_key_seen;
static bool s_clock_observer_owned;
static uint32_t pending_status;
static int level, hold_error, install_error, add_error, type_error, enable_error;
static int installs, removes, uninstalls, disables;
static void (*handler)(void *);
#define REG_READ(reg) (pending_status)
static int gpio_get_level(int pin) { assert(pin == BSP_BTN_GPIO); return level; }
static int gpio_hold_dis(int pin) { assert(pin == BSP_BTN_GPIO); return hold_error; }
static int gpio_config(const gpio_config_t *cfg) {
    assert(cfg->pin_bit_mask == 1 && cfg->mode == GPIO_MODE_INPUT && !cfg->pull_up_en && !cfg->pull_down_en && cfg->intr_type == GPIO_INTR_DISABLE);
    return 0;
}
static int gpio_install_isr_service(int flags) { assert(flags == ESP_INTR_FLAG_IRAM); ++installs; return install_error; }
static int gpio_isr_handler_add(int pin, void (*callback)(void *), void *argument) { assert(pin == 0 && !argument); handler = callback; return add_error; }
static int gpio_set_intr_type(int pin, int type) { assert(pin == 0 && type == GPIO_INTR_NEGEDGE); return type_error; }
static int gpio_intr_enable(int pin) { assert(pin == 0); return enable_error; }
static int gpio_intr_disable(int pin) { assert(pin == 0); ++disables; return 0; }
static int gpio_isr_handler_remove(int pin) { assert(pin == 0); ++removes; return 0; }
static void gpio_uninstall_isr_service(void) { ++uninstalls; }
'''
        for signature, name in (("static void clock_key_isr(void *argument)", "clock_key_isr"),
                                ("static bool clock_key_pending(void)", "clock_key_pending"),
                                ("static void clock_key_observer_end(void)", "clock_key_observer_end"),
                                ("static bool clock_key_observer_begin(void)", "clock_key_observer_begin")):
            source += signature + " {\n" + function_body(main, name) + "\n}\n"
        source += r'''
static void reset(void) {
    s_clock_key_seen = s_clock_observer_owned = false;
    pending_status = 0; level = 1;
    hold_error = install_error = add_error = type_error = enable_error = 0;
    installs = removes = uninstalls = disables = 0; handler = NULL;
}
int main(void) {
    reset(); assert(clock_key_observer_begin() && handler && !clock_key_pending());
    handler(NULL); assert(level == 1 && !pending_status && clock_key_pending());
    assert(clock_key_pending()); clock_key_observer_end(); clock_key_observer_end();
    assert(removes == 1 && uninstalls == 1 && disables == 1);
    reset(); pending_status = 1; assert(!clock_key_observer_begin() && !installs);
    reset(); level = 0; assert(!clock_key_observer_begin() && !installs);
    reset(); hold_error = -1; assert(!clock_key_observer_begin() && !installs);
    reset(); install_error = -1; assert(!clock_key_observer_begin());
    clock_key_observer_end(); assert(installs == 1 && !uninstalls && !removes);
    for (int failure = 0; failure < 3; ++failure) {
        reset();
        if (!failure) add_error = -1; else if (failure == 1) type_error = -1; else enable_error = -1;
        assert(!clock_key_observer_begin()); clock_key_observer_end();
        assert(!s_clock_observer_owned && uninstalls == 1 && removes == 1);
    }
    reset(); assert(clock_key_observer_begin()); pending_status = 1;
    assert(clock_key_pending()); pending_status = 0; assert(clock_key_pending());
    clock_key_observer_end();
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rtc-key-observer-", dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            test = Path(directory) / "observer.c"
            output = Path(directory) / "observer"
            test.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)

    def test_terminal_anchor_follows_hardware_handoff(self):
        body = function_body((ROOT / "main/main.c").read_text(), "system_enter_deep_sleep")
        self.assertIn("rtc_clock_prepare_sleep()", body)
        self.assertIn("rtc_clock_commit_sleep(", body)
        self.assertLess(body.index("rtc_clock_prepare_sleep()"), body.index("bsp_lvgl_lock("))
        for handoff in ("bsp_button_prepare_deep_sleep(", "bsp_i2c_prepare_deep_sleep()", "bsp_display_prepare_deep_sleep()"):
            self.assertLess(body.index(handoff), body.index("rtc_clock_commit_sleep("))
        self.assertLess(body.index("rtc_clock_commit_sleep("), body.index("esp_deep_sleep_start()"))


if __name__ == "__main__":
    unittest.main()
