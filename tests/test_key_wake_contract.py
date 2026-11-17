#!/usr/bin/env python3
"""启动守卫的单写者接线约束，并运行真实 on_key 验证空闲通知不会进入应用队列。"""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class KeyWakeGuardWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = read("main/main.c")
        cls.bsp = read("components/bsp/src/bsp_button.c")

    def test_on_key_consumes_the_guard_before_the_readiness_check(self) -> None:
        # 初始化尚未开放输入时，空闲回调也必须能撤防，不能被 readiness 提前丢弃。
        body = function_body(self.main, "on_key")
        self.assertRegex(body, r"key_guard_consume\(&s_key_guard,\s*ev == BSP_BTN_IDLE\)")
        self.assertLess(body.index("key_guard_consume("), body.index("!s_input_ready"))
        self.assertLess(body.index("key_guard_consume("), body.index("xQueueSend("))
        self.assertNotIn("BSP_BTN_RELEASE", body)

    def test_all_boots_arm_before_init_without_main_task_resynchronization(self) -> None:
        body = function_body(self.main, "app_main")
        arm = "key_guard_arm(&s_key_guard);"
        self.assertEqual(body.count(arm), 1)
        self.assertLess(body.index(arm), body.index("bsp_button_init("))
        # 武装必须在函数顶层，不能只位于 GPIO 唤醒分支。
        prefix = body[:body.index(arm)]
        self.assertEqual(prefix.count("{"), prefix.count("}"))
        prefix = re.sub(r"//[^\n]*|/\*[\s\S]*?\*/", "", prefix)
        self.assertRegex(prefix.rstrip(), r"[;}]$")
        self.assertRegex(body, r"(?m)^    key_guard_arm\(&s_key_guard\);$")
        self.assertNotIn("s_key_guard", body[body.index("bsp_button_init("):])
        self.assertNotIn("bsp_button_read_mv(", body)
        self.assertNotIn("KEY_GUARD_MAX_US", self.main)

    def test_guard_has_one_timer_context_writer_after_init_starts(self) -> None:
        remainder = self.main
        for name in ("app_main", "on_key"):
            remainder = remainder.replace(function_body(self.main, name), "")
        remainder = re.sub(r"static key_guard_t s_key_guard\s*;", "", remainder)
        self.assertNotIn("s_key_guard", remainder)
        self.assertEqual(self.main.count("key_guard_arm("), 1)
        self.assertEqual(self.main.count("key_guard_consume("), 1)
        api = re.findall(r"static inline \w+ (key_guard_\w+)\(", read("main/key_guard.h"))
        self.assertEqual(sorted(api), ["key_guard_arm", "key_guard_consume"])
        self.assertNotRegex(read("main/key_guard.h"), r"deadline|timeout|volatile|now_us")

    def test_bsp_uses_public_gesture_end_without_extra_polling_or_power_save(self) -> None:
        body = function_body(self.bsp, "register_callbacks")
        self.assertEqual(body.count("iot_button_register_cb("), 6)
        self.assertRegex(body, r"BUTTON_PRESS_END,\s*NULL,\s*cb_end,\s*index")
        self.assertNotRegex(self.bsp, r"(?:xTaskCreate\w*|esp_timer_create|malloc|calloc|realloc)\s*\(")
        self.assertNotRegex(self.bsp, r"enable_power_save\s*=\s*(?:true|1)|iot_button_\w*power_save")
        self.assertNotRegex(self.bsp, r"iot_button_get_(?:event|key_level|pressed_time)\s*\(")
        code = re.sub(r"//[^\n]*|/\*[\s\S]*?\*/", "", self.bsp)
        self.assertEqual(code.count("adc_oneshot_read("), 2)
        self.assertEqual(code.count("adc_oneshot_new_unit("), 1)
        self.assertIn("CONFIG_BUTTON_PERIOD_TIME_MS", self.bsp)
        self.assertIn("CONFIG_BUTTON_DEBOUNCE_TICKS", self.bsp)

    def test_actual_on_key_discards_every_idle_and_accepts_next_gesture(self) -> None:
        # 提取真实函数体编译；只替代 SDK 队列与时钟，不复制被测接线逻辑。
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "bsp_button.h"
#include "key_guard.h"
typedef struct {
    int type;
    struct { struct { bsp_btn_t btn; bsp_btn_ev_t ev; uint64_t when_ms; } key; } data;
} app_event_t;
enum { EVT_KEY };
static key_guard_t s_key_guard;
static bool s_input_ready;
static void *s_evt_queue;
static int queued;
static int64_t clock_us;
#define ESP_LOGI(...) ((void)0)
static int64_t esp_timer_get_time(void) { return clock_us; }
static int xQueueSend(void *queue, const app_event_t *event, int wait) {
    assert(queue && wait == 0 && event->type == EVT_KEY);
    assert(event->data.key.ev != BSP_BTN_IDLE);
    ++queued;
    return 1;
}
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
ON_KEY_BODY
}
int main(void) {
    key_guard_arm(&s_key_guard);
    on_key(BSP_BTN_OK, BSP_BTN_IDLE, 0);
    assert(!s_key_guard.armed && queued == 0);
    s_input_ready = true;
    s_evt_queue = &queued;
    key_guard_arm(&s_key_guard);
    on_key(BSP_BTN_OK, BSP_BTN_PRESS, 0);
    clock_us = 11000000;
    on_key(BSP_BTN_OK, BSP_BTN_LONG, 0);
    on_key(BSP_BTN_OK, BSP_BTN_RELEASE, 0);
    on_key(BSP_BTN_OK, BSP_BTN_CLICK, 0);
    on_key(BSP_BTN_OK, BSP_BTN_DOUBLE, 0);
    assert(s_key_guard.armed && queued == 0);
    on_key(BSP_BTN_UP, BSP_BTN_PRESS, 0);
    on_key(BSP_BTN_OK, BSP_BTN_IDLE, 0);
    assert(!s_key_guard.armed && queued == 0);
    on_key(BSP_BTN_OK, BSP_BTN_IDLE, 0);
    on_key(BSP_BTN_UP, BSP_BTN_IDLE, 0);
    assert(queued == 0);
    on_key(BSP_BTN_DOWN, BSP_BTN_PRESS, 0);
    on_key(BSP_BTN_DOWN, BSP_BTN_RELEASE, 0);
    on_key(BSP_BTN_DOWN, BSP_BTN_CLICK, 0);
    assert(queued == 3);
    return 0;
}
'''.replace("ON_KEY_BODY", function_body(self.main, "on_key"))
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as directory:
            source = Path(directory) / "on_key.c"
            binary = Path(directory) / "on_key"
            source.write_text(harness, encoding="utf-8")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main"),
                "-I" + str(ROOT / "components/bsp/include"),
                "-I" + str(ROOT / "tests/bsp_stubs"),
                str(source), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
