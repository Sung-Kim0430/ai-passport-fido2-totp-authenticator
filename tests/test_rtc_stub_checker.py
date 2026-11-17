#!/usr/bin/env python3
"""检查审计器自身的拒绝规则，真实固件仍必须再用最终 ELF 审计。"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_rtc_stub import audit_disassembly
import check_rtc_stub as checker


class RtcAuditTest(unittest.TestCase):
    def test_generated_configuration_controls_required_entry(self):
        self.assertTrue(hasattr(checker, "maintenance_enabled"))
        self.assertTrue(checker.maintenance_enabled("CONFIG_AUTH_RTC_WAKE_STUB=y\n"))
        self.assertFalse(checker.maintenance_enabled("# CONFIG_AUTH_RTC_WAKE_STUB is not set\n"))
        self.assertFalse(checker.maintenance_enabled("# CONFIG_AUTH_RTC_WAKE_STUB=y\n"))

    def test_rtc_and_documented_rom_calls(self):
        text = '''
50000000 <rtc_clock_wake_stub>:
50000000: 010000ef jal 50000010 <helper>
50000004: 8082 ret
50000010 <helper>:
50000010: 00000097 auipc ra,0x0
50000014: 000080e7 jalr ra # 40000040 <esp_rom_delay_us>
50000018: 8082 ret
'''
        rtc, rom = audit_disassembly(text)
        self.assertEqual(rtc, {"rtc_clock_wake_stub", "helper"})
        self.assertEqual(rom, {"esp_rom_delay_us"})

    def test_reject_flash_iram_and_unapproved_rom(self):
        for address, name in (("42000100", "flash_fn"), ("40380000", "iram_fn"),
                              ("40000050", "printf")):
            text = f'''50000000 <rtc_clock_wake_stub>:
50000000: 000080e7 jalr ra # {address} <{name}>
'''
            with self.subTest(address=address), self.assertRaises(ValueError):
                audit_disassembly(text)

    def test_reject_unresolved_indirect_call(self):
        with self.assertRaises(ValueError):
            audit_disassembly('''50000000 <rtc_clock_wake_stub>:
50000000: 9782 jalr a5
''')

    def test_reject_flash_and_ordinary_ram_data(self):
        for address in ("3c000010", "3fc80000"):
            text = f'''50000000 <rtc_clock_wake_stub>:
50000000: 0007a783 lw a5,0(a5) # {address} <bad_data>
'''
            with self.subTest(address=address), self.assertRaises(ValueError):
                audit_disassembly(text)
        with self.assertRaises(ValueError):
            audit_disassembly('''50000000 <rtc_clock_wake_stub>:
50000000: 3c0007b7 lui a5,0x3c000
''')

    def test_local_branches_and_rtc_data(self):
        text = '''50000000 <rtc_clock_wake_stub>:
50000000: 0007a783 lw a5,0(a5) # 50001000 <rtc_clock_retained>
50000004: 00000063 beqz a0,50000000 <rtc_clock_wake_stub>
50000008: 8082 ret
'''
        self.assertEqual(audit_disassembly(text), ({"rtc_clock_wake_stub"}, set()))

    def test_reject_missing_or_misplaced_entry(self):
        for text in ("", "42000000 <rtc_clock_wake_stub>:\n42000000: 8082 ret\n"):
            with self.assertRaises(ValueError):
                audit_disassembly(text)

    def test_reject_undeclared_rtc_target(self):
        with self.assertRaises(ValueError):
            audit_disassembly('''50000000 <rtc_clock_wake_stub>:
50000000: 020000ef jal 50000020 <missing>
''')


if __name__ == "__main__":
    unittest.main()
