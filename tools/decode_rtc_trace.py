#!/usr/bin/env python3
"""Decode a local rtc_trace_store memory dump; never connects to a device."""
import argparse
import json
from pathlib import Path
import struct
import zlib

CAPACITY = 44
SIZE = 6152
Q19 = 1 << 19
TICK_MASK = (1 << 48) - 1
KINDS = {1: "reset", 2: "sleep", 3: "reference", 4: "gap", 5: "uncertain_reference"}
FIELDS = (
    "sequence", "kind", "start_tick", "wake_tick", "end_tick", "hrt_us", "commit_hrt_us",
    "wall_before_us", "wall_after_us", "raw_us", "corrected_us", "awake_us",
    "light_raw_us", "light_sdk_us", "light_offset_us", "period_q19", "ppm",
    "raw_fraction", "continuity", "source", "checksum",
)
FORMAT = struct.Struct("<IIQQQqqqqQQQQQqIiIIII")
assert FORMAT.size == 136


def decode(data):
    if len(data) != SIZE:
        raise ValueError(f"expected {SIZE} bytes, got {len(data)}")
    magic, version, sequence, crc = struct.unpack_from("<IIII", data)
    if magic != 0x52545431 or version != 1:
        raise ValueError("unsupported trace magic/version")
    header_valid = zlib.crc32(data[:12]) == crc
    rows, invalid = [], []
    for index in range(CAPACITY):
        raw = data[16 + index * 136:16 + (index + 1) * 136]
        row = dict(zip(FIELDS, FORMAT.unpack(raw)))
        if not any(raw):
            continue
        if (not row["sequence"] or row["kind"] not in KINDS or
                (row["sequence"] - 1) % CAPACITY != index or
                zlib.crc32(raw[:132]) != row["checksum"]):
            invalid.append(index)
            continue
        row.pop("checksum")
        row["kind"] = KINDS[row["kind"]]
        if row["kind"] == "sleep":
            start, end, wake = row["start_tick"], row["end_tick"], row["wake_tick"]
            period = row["period_q19"]
            delta = (end - start) & TICK_MASK
            valid = (start <= TICK_MASK and end <= TICK_MASK and delta < (1 << 47) and
                     2 * Q19 <= period <= 20 * Q19)
            row["tick_span_valid"] = valid
            if valid:
                row["tick_elapsed_us"], row["tick_fraction_q19"] = divmod(delta * period, Q19)
                if wake <= TICK_MASK and ((wake - start) & TICK_MASK) <= delta:
                    row["wake_to_app_us"] = ((end - wake) & TICK_MASK) * period // Q19
        if row["kind"] in ("reference", "uncertain_reference") and row["wall_before_us"] >= 0:
            row["sync_adjustment_us"] = row["wall_after_us"] - row["wall_before_us"]
        rows.append(row)
    rows.sort(key=lambda row: row["sequence"])
    for previous, row in zip(rows, rows[1:]):
        if (row["sequence"] != previous["sequence"] + 1 or
                row["kind"] != "sleep" or previous["kind"] != "sleep" or
                row["continuity"] != previous["continuity"] or row["source"] != previous["source"] or
                not row.get("tick_span_valid")):
            continue
        awake = row["commit_hrt_us"] - previous["hrt_us"] - row["light_sdk_us"]
        row["independent_awake_us"] = awake
        row["awake_balance_us"] = row["awake_us"] - previous["awake_us"] - awake
        delta_raw_q19 = (row["raw_us"] - previous["raw_us"]) * Q19 + row["raw_fraction"] - previous["raw_fraction"]
        deep_q19 = row["tick_elapsed_us"] * Q19 + row["tick_fraction_q19"]
        # Boot-local light fractions are not stored; the remainder must be in
        # [0, Q19). A larger discrepancy is an accounting mismatch, not ppm.
        row["unattributed_raw_q19"] = delta_raw_q19 - deep_q19 - row["light_raw_us"] * Q19
        row["raw_balance_valid"] = 0 <= row["unattributed_raw_q19"] < Q19
    pending_bytes = data[16 + CAPACITY * FORMAT.size:16 + (CAPACITY + 1) * FORMAT.size]
    pending = dict(zip(FIELDS, FORMAT.unpack(pending_bytes)))
    if (not pending["sequence"] or pending["kind"] != 2 or
            zlib.crc32(pending_bytes[:132]) != pending["checksum"]):
        pending = None
    else:
        pending.pop("checksum")
        pending["kind"] = "pending_sleep"
        wake, inverse = struct.unpack_from("<QQ", data, SIZE - 16)
        pending["wake_tick"] = wake if wake ^ inverse == (1 << 64) - 1 else None
    latest = max([row["sequence"] for row in rows] + ([sequence] if header_valid else [0]))
    first = max(1, latest - CAPACITY + 1)
    present = {row["sequence"] for row in rows}
    return {
        "version": version, "header_valid": header_valid,
        "overwritten_records": max(0, latest - CAPACITY),
        "invalid_slots": invalid,
        "missing_sequences": [number for number in range(first, latest + 1) if number not in present],
        "pending": pending,
        "records": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(decode(args.dump.read_bytes()), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
