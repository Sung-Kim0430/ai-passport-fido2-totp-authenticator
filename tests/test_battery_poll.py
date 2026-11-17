#!/usr/bin/env python3
"""编译真实帧电量查询函数，验证低频 I²C 与 USB 边沿策略。"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_key_wake_contract import function_body

ROOT = Path(__file__).resolve().parents[1]


class BatteryPollTest(unittest.TestCase):
    def test_frame_uses_cached_query(self):
        body = function_body((ROOT / "main/main.c").read_text(), "refresh_display")
        self.assertIn("battery_soc_for_frame(now_ms, is_charging)", body)
        self.assertNotIn("bsp_battery_soc()", body)

    def test_bounded_poll_with_immediate_usb_edge(self):
        body = function_body((ROOT / "main/main.c").read_text(), "battery_soc_for_frame")
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
static unsigned reads;
static int8_t value = 42;
static int8_t bsp_battery_soc(void) { ++reads; return value; }
static int8_t battery_soc_for_frame(uint64_t now_ms, bool usb_connected) {
''' + body + r'''
}
int main(void) {
    assert(battery_soc_for_frame(0, false) == 42 && reads == 1);
    assert(battery_soc_for_frame(4999, false) == 42 && reads == 1);
    assert(battery_soc_for_frame(5000, false) == 42 && reads == 2);
    value = 61;
    assert(battery_soc_for_frame(5001, true) == 61 && reads == 3);
    assert(battery_soc_for_frame(5002, true) == 61 && reads == 3);
    value = -1;
    assert(battery_soc_for_frame(5003, false) == -1 && reads == 4);
    assert(battery_soc_for_frame(10002, false) == -1 && reads == 4);
    assert(battery_soc_for_frame(10003, false) == -1 && reads == 5);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get("COMMANDCODE_SCRATCHPAD")) as folder:
            c_file = Path(folder) / "battery_poll.c"
            binary = Path(folder) / "battery_poll"
            c_file.write_text(source)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(c_file), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
