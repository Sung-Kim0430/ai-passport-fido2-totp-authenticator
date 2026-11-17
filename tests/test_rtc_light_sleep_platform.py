#!/usr/bin/env python3
"""Compile the actual ESP-IDF adapter against narrow, deterministic SDK stubs."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#ifndef TEST_SLEEP_SDK_H
#define TEST_SLEEP_SDK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* newlib declares this GNU extension through FreeRTOS headers. */
int on_exit(void (*function)(int, void *), void *arg);
#define CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT 1
#define CONFIG_ESP_SLEEP_DEBUG 1
#define CONFIG_PM_ENABLE 1
#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 1
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 1
#define IRAM_ATTR
#define DRAM_ATTR
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
typedef int esp_err_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern int lock_depth;
#define portENTER_CRITICAL(p) ((void)(p), ++lock_depth)
#define portEXIT_CRITICAL(p) ((void)(p), --lock_depth)
typedef struct {
 uint32_t lightsleep_cnt;
 uint64_t sleep_in_rtc_time_stamp, sleep_out_rtc_time_stamp;
 uint32_t wakeup_triggers, sleep_flags;
 esp_err_t sleep_request_result;
} esp_sleep_context_t;
typedef esp_err_t (*esp_pm_light_sleep_cb_t)(int64_t, void *);
typedef struct {
 esp_pm_light_sleep_cb_t enter_cb, exit_cb;
 void *enter_cb_user_arg, *exit_cb_user_arg;
 uint32_t enter_cb_prior, exit_cb_prior;
} esp_pm_sleep_cbs_register_config_t;
typedef bool (*skip_light_sleep_cb_t)(void);
esp_err_t esp_pm_register_skip_light_sleep_callback(skip_light_sleep_cb_t cb);
esp_err_t esp_pm_unregister_skip_light_sleep_callback(skip_light_sleep_cb_t cb);
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *config);
esp_err_t esp_pm_light_sleep_unregister_cbs(esp_pm_sleep_cbs_register_config_t *config);
void esp_sleep_set_sleep_context(esp_sleep_context_t *ctx);
int64_t esp_timer_get_time(void);
uint32_t esp_clk_slowclk_cal_get(void);
uint32_t rtc_clk_slow_src_get(void);
#endif
'''
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_sleep_sdk.h"
#include "rtc_light_sleep.h"
int lock_depth;
int64_t s_microseconds_offset;
static int64_t next_offset_delta = 100;
static int64_t now = 100;
static uint32_t source = 0, sdk_period = 6 * RTC_DRIFT_Q19;
static skip_light_sleep_cb_t skip;
static esp_pm_sleep_cbs_register_config_t callbacks;
static esp_sleep_context_t *context;
static int fail_skip, fail_pm, unregister_pm, unregister_skip;
esp_err_t esp_pm_register_skip_light_sleep_callback(skip_light_sleep_cb_t cb) {
 if (fail_skip) return ESP_ERR_NO_MEM;
 skip = cb; return ESP_OK;
}
esp_err_t esp_pm_unregister_skip_light_sleep_callback(skip_light_sleep_cb_t cb) {
 assert(cb == skip); skip = NULL; ++unregister_skip; return ESP_OK;
}
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *conf) {
 callbacks.enter_cb = conf->enter_cb; /* IDF can fail AFTER adding enter */
 if (fail_pm) return ESP_ERR_NO_MEM;
 callbacks = *conf; return ESP_OK;
}
esp_err_t esp_pm_light_sleep_unregister_cbs(esp_pm_sleep_cbs_register_config_t *conf) {
 assert(conf->enter_cb == callbacks.enter_cb);
 memset(&callbacks, 0, sizeof(callbacks)); ++unregister_pm; return ESP_OK;
}
void esp_sleep_set_sleep_context(esp_sleep_context_t *ctx) { context = ctx; }
int64_t esp_timer_get_time(void) { return now; }
uint32_t esp_clk_slowclk_cal_get(void) { return sdk_period; }
uint32_t rtc_clk_slow_src_get(void) { return source; }
static void enter(void) { assert(callbacks.enter_cb(5000, callbacks.enter_cb_user_arg) == ESP_OK); }
static void exit_sleep(void) { assert(callbacks.exit_cb(9999, callbacks.exit_cb_user_arg) == ESP_OK); }
static void successful_sleep(void) {
 enter(); context->sleep_in_rtc_time_stamp = 10;
 context->sleep_out_rtc_time_stamp = 20; context->sleep_request_result = ESP_OK;
 s_microseconds_offset += next_offset_delta;
 now += 100; exit_sleep();
}
int main(int argc, char **argv) {
 assert(argc == 2);
 if (!strcmp(argv[1], "skip_failure")) {
  fail_skip = 1; assert(!rtc_light_sleep_platform_start(sdk_period, source));
  assert(!context && !skip && !callbacks.enter_cb); return 0;
 }
 if (!strcmp(argv[1], "pm_failure")) {
  fail_pm = 1; assert(!rtc_light_sleep_platform_start(sdk_period, source));
  assert(!context && !skip && !callbacks.enter_cb);
  assert(unregister_pm == 1 && unregister_skip == 1); return 0;
 }
 assert(rtc_light_sleep_platform_start(sdk_period + RTC_DRIFT_Q19 / 2, source));
 assert(!skip()); successful_sleep();
 rtc_light_sleep_snapshot_t s = rtc_light_sleep_platform_snapshot();
 assert(s.sleep_count == 1 && s.raw_total.us == 65 && s.sdk_total_us == 60);
 assert(s.last_exit_hrt_us == now && s.hrt_us == now && !s.fault);
 assert(s.wall_offset_total_us == 100);
 if (!strcmp(argv[1], "fresh")) {
  enter(); s_microseconds_offset += 1000; now += 5; exit_sleep(); /* PM skips SDK entirely */
  s = rtc_light_sleep_platform_snapshot();
  assert(s.wall_offset_total_us == 100);
  assert(s.sleep_count == 1 && s.raw_total.us == 65 && s.reject_count == 0);
  enter(); s_microseconds_offset -= 500;
  context->sleep_request_result = ESP_FAIL; now += 1; exit_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.sleep_count == 1 && s.reject_count == 1 && s.raw_total.us == 65);
  assert(s.wall_offset_total_us == 100);
  next_offset_delta = -150; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.wall_offset_total_us == -50 && s.sleep_count == 2);
 } else if (!strcmp(argv[1], "wall_delta_overflow")) {
  s_microseconds_offset = INT64_MIN; enter();
  s_microseconds_offset = INT64_MAX;
  context->sleep_in_rtc_time_stamp = 30; context->sleep_out_rtc_time_stamp = 40;
  context->sleep_request_result = ESP_OK; now += 100; exit_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.wall_offset_total_us == 100 && s.sleep_count == 1);
  assert(s.raw_total.us == 65 && s.sdk_total_us == 60 && skip());
 } else if (!strcmp(argv[1], "wall_delta_underflow")) {
  s_microseconds_offset = INT64_MAX; enter();
  s_microseconds_offset = INT64_MIN;
  context->sleep_in_rtc_time_stamp = 30; context->sleep_out_rtc_time_stamp = 40;
  context->sleep_request_result = ESP_OK; now += 100; exit_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.wall_offset_total_us == 100 && s.sleep_count == 1);
  assert(s.raw_total.us == 65 && s.sdk_total_us == 60 && skip());
 } else if (!strcmp(argv[1], "wall_total_underflow")) {
  s_microseconds_offset = 0; next_offset_delta = INT64_MIN; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.wall_offset_total_us == INT64_MIN + 100 && s.sleep_count == 2);
  s_microseconds_offset = 0; next_offset_delta = -101; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.wall_offset_total_us == INT64_MIN + 100 && s.sleep_count == 2);
  assert(s.raw_total.us == 130 && s.sdk_total_us == 120 && skip());
 } else if (!strcmp(argv[1], "wall_total_overflow")) {
  next_offset_delta = INT64_MAX - 100; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.wall_offset_total_us == INT64_MAX && s.sleep_count == 2);
  s_microseconds_offset = 0; next_offset_delta = 1; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.wall_offset_total_us == INT64_MAX && s.sleep_count == 2);
  assert(s.raw_total.us == 130 && s.sdk_total_us == 120 && skip());
 } else if (!strcmp(argv[1], "incomplete")) {
  enter(); context->sleep_request_result = ESP_OK; now += 1; exit_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.sleep_count == 1 && s.sdk_total_us == 60 && skip());
 } else if (!strcmp(argv[1], "source_exit")) {
  enter(); ++source; context->sleep_request_result = ESP_OK;
  context->sleep_in_rtc_time_stamp = 30; context->sleep_out_rtc_time_stamp = 40;
  now += 100; exit_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.sleep_count == 1 && s.sdk_total_us == 60 && skip());
 } else if (!strcmp(argv[1], "sdk_invalid")) {
  sdk_period = 0; successful_sleep();
  s = rtc_light_sleep_platform_snapshot();
  assert(s.fault && s.sleep_count == 1 && s.sdk_total_us == 60 && skip());
 } else if (!strcmp(argv[1], "stale")) {
  now += 120000001; assert(skip());
  rtc_light_sleep_platform_set_period(sdk_period); assert(!skip());
 } else if (!strcmp(argv[1], "source")) {
  ++source; assert(skip()); assert(rtc_light_sleep_platform_snapshot().fault);
  --source; rtc_light_sleep_platform_set_period(sdk_period); assert(skip());
 } else if (!strcmp(argv[1], "inhibit")) {
  rtc_light_sleep_platform_inhibit(); assert(skip());
  rtc_light_sleep_platform_set_period(sdk_period); assert(skip());
 } else if (!strcmp(argv[1], "invalid")) {
  rtc_light_sleep_platform_set_period(0); assert(skip());
  assert(rtc_light_sleep_platform_snapshot().fault);
 } else assert(0);
 assert(lock_depth == 0);
 puts("RTC light sleep platform: PASS"); return 0;
}
'''

class PlatformTests(unittest.TestCase):
    def test_actual_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / 'test_sleep_sdk.h').write_text(STUB)
            for name in ('sdkconfig.h', 'esp_attr.h', 'esp_err.h', 'esp_pm.h',
                         'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/portmacro.h',
                         'esp_private/pm_impl.h', 'esp_private/esp_clk.h',
                         'esp_private/esp_sleep_internal.h', 'soc/rtc.h'):
                path = directory / name
                path.parent.mkdir(exist_ok=True, parents=True)
                path.write_text('#include "test_sleep_sdk.h"\n')
            (directory / 'harness.c').write_text(HARNESS)
            binary = directory / 'adapter'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', '-I', str(directory), '-I', str(ROOT / 'main'),
                            str(directory / 'harness.c'),
                            str(ROOT / 'main/rtc_light_sleep_platform.c'),
                            '-o', str(binary)], check=True)
            for scenario in ('fresh', 'stale', 'source', 'inhibit', 'invalid',
                             'skip_failure', 'pm_failure', 'incomplete', 'source_exit', 'sdk_invalid',
                             'wall_delta_overflow', 'wall_total_overflow',
                             'wall_delta_underflow', 'wall_total_underflow'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True)

if __name__ == '__main__':
    unittest.main()
