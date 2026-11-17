#!/usr/bin/env python3
"""检查最终 C3 RTC 调用/数据引用；不替代寄存器语义、脉宽和功耗验收。"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

RTC_START = 0x50000000
RTC_END = RTC_START + 8192
ROM_START = 0x40000000
ROM_END = 0x40080000
ROM_ALLOWED = {
    "esp_rom_delay_us", "ets_delay_us", "set_rtc_memory_crc",
    "__udivdi3", "__umoddi3", "__muldi3", "__udivmoddi4", "__udivsi3", "__umodsi3",
}
LABEL = re.compile(r"^\s*([0-9a-fA-F]+) <([^>]+)>:$")
INSTRUCTION = re.compile(r"^\s*([0-9a-fA-F]+):\s+(?:(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{4}|[0-9a-fA-F]{2})\s+)+([a-z][a-z0-9._]*)\s*(.*)$")
REFERENCE = re.compile(r"\b([0-9a-fA-F]{8,16})\s*<([^>]+)>")


def forbidden_data(address):
    return 0x3C000000 <= address < 0x3E000000 or 0x3FC00000 <= address < 0x40000000


def symbol_base(name):
    return re.sub(r"\+0x[0-9a-fA-F]+$", "", name)


def parse_functions(text):
    functions = {}
    current = None
    for line in text.splitlines():
        label = LABEL.match(line)
        if label:
            current = label.group(2)
            functions[current] = (int(label.group(1), 16), [])
            continue
        instruction = INSTRUCTION.match(line)
        if current is not None and instruction:
            functions[current][1].append((int(instruction.group(1), 16), instruction.group(2), instruction.group(3)))
    return functions


def audit_disassembly(text, entry="rtc_clock_wake_stub"):
    functions = parse_functions(text)
    if entry not in functions:
        raise ValueError(f"RTC entry missing: {entry}")
    reachable, rom = set(), set()
    pending = [entry]
    while pending:
        name = pending.pop()
        if name in reachable:
            continue
        address, instructions = functions[name]
        if not RTC_START <= address < RTC_END:
            raise ValueError(f"non-RTC function: {name} at {address:#x}")
        if not instructions:
            raise ValueError(f"undecoded RTC function: {name}")
        reachable.add(name)
        for pc, mnemonic, operands in instructions:
            references = [(int(a, 16), symbol_base(n)) for a, n in REFERENCE.findall(operands)]
            for target, target_name in references:
                if forbidden_data(target):
                    raise ValueError(f"ordinary RAM/flash reference: {name}+{pc-address:#x} -> {target_name} {target:#x}")
            # 无符号的绝对地址装载也不能偷偷引用 DROM 或普通 DRAM。
            if mnemonic == "lui":
                immediate = re.search(r",\s*(?:0x)?([0-9a-fA-F]+)\b", operands)
                if immediate and forbidden_data(int(immediate.group(1), 16) << 12):
                    raise ValueError(f"forbidden address materialization at {pc:#x}")
            flow = mnemonic in {"jal", "jalr", "j", "jr", "call", "tail"} or mnemonic.startswith("b")
            if not flow:
                continue
            if mnemonic == "jr" and operands.strip() == "ra":
                continue
            if not references:
                raise ValueError(f"unresolved control transfer at {pc:#x}: {mnemonic} {operands}")
            for target, target_name in references:
                if target_name == name:
                    continue
                if RTC_START <= target < RTC_END:
                    if target_name not in functions:
                        raise ValueError(f"undeclared RTC target at {pc:#x}: {target_name}")
                    pending.append(target_name)
                elif ROM_START <= target < ROM_END and target_name in ROM_ALLOWED:
                    rom.add(target_name)
                else:
                    raise ValueError(f"unapproved target at {pc:#x}: {target_name} {target:#x}")
    return reachable, rom


def maintenance_enabled(text):
    return bool(re.search(r"^CONFIG_AUTH_RTC_WAKE_STUB=y$", text, re.MULTILINE))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--objdump", default="riscv32-esp-elf-objdump")
    parser.add_argument("--optional", action="store_true")
    parser.add_argument("--sdkconfig", type=Path)
    args = parser.parse_args()
    try:
        required = args.sdkconfig is not None and maintenance_enabled(args.sdkconfig.read_text(encoding="utf-8"))
        optional = args.optional or (args.sdkconfig is not None and not required)
        text = subprocess.check_output([args.objdump, "-d", "-j", ".rtc.text", str(args.elf)], text=True)
        if "<rtc_clock_wake_stub>:" not in text and optional and not required:
            print("RTC stub audit: NOT ENABLED (no maintenance entry)")
            return 0
        rtc, rom = audit_disassembly(text)
        print(f"RTC stub static audit: PASS ({len(rtc)} RTC functions; ROM: {', '.join(sorted(rom))})")
        print("Unverified: hardware GPIO pulse coverage, re-sleep timing, power and long-term drift")
        return 0
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"RTC stub audit: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
