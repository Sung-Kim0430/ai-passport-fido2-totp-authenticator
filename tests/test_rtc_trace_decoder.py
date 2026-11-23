import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('rtc_trace_decoder', ROOT / 'tools/decode_rtc_trace.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def record(sequence=1, kind=2, start=100, end=100000100, raw=1500000000):
    data = bytearray(136)
    struct.pack_into('<IIQQQqqqqQQQQQqIiIII', data, 0,
                     sequence, kind, start, start + 90000000, end, 12345, 1000,
                     1700000000000000, 1700001500000000,
                     raw, raw + 1000, 55, 0, 0, 0, 15 * 524288,
                     -870, 0, 1, 2)
    struct.pack_into('<I', data, 132, zlib.crc32(data[:132]))
    return data


def blob(rows):
    data = bytearray(6152)
    struct.pack_into('<III', data, 0, 0x52545431, 1, max((seq for seq, _ in rows), default=0))
    struct.pack_into('<I', data, 12, zlib.crc32(data[:12]))
    for seq, row in rows:
        index = (seq - 1) % 44
        data[16 + index * 136:16 + (index + 1) * 136] = row
    return data


class DecoderTests(unittest.TestCase):
    def test_reconstruction_and_missing_records_are_explicit(self):
        result = decoder.decode(blob([(2, record(2)), (4, record(4))]))
        self.assertEqual([row['sequence'] for row in result['records']], [2, 4])
        self.assertEqual(result['missing_sequences'], [1, 3])
        self.assertEqual(result['records'][0]['tick_elapsed_us'], 1500000000)
        self.assertEqual(result['records'][0]['wake_to_app_us'], 150000000)

    def test_corruption_size_and_version(self):
        row = record()
        row[56] ^= 1
        result = decoder.decode(blob([(1, row)]))
        self.assertEqual(result['records'], [])
        self.assertEqual(result['invalid_slots'], [0])
        for data in (bytes(3), bytes(6152), blob([]) + b'x'):
            with self.assertRaises(ValueError):
                decoder.decode(data)

    def test_wrapping_does_not_claim_full_coverage(self):
        result = decoder.decode(blob([(n, record(n)) for n in range(10, 54)]))
        self.assertEqual(result['overwritten_records'], 9)
        self.assertEqual(result['missing_sequences'], [])

    def test_reference_error(self):
        row = record(kind=3)
        struct.pack_into('<q', row, 56, 1700000420000000)
        struct.pack_into('<I', row, 132, zlib.crc32(row[:132]))
        self.assertEqual(decoder.decode(blob([(1, row)]))['records'][0]['sync_adjustment_us'], 420000000)

    def test_pending_is_visible_and_awake_is_independent(self):
        first = record(1, raw=1500000000)
        second = record(2, raw=3000300000)
        struct.pack_into('<q', second, 40, 12345 + 7000000 + 300000)
        struct.pack_into('<Q', second, 80, 55 + 7000000)
        struct.pack_into('<QQ', second, 88, 300000, 300000)
        struct.pack_into('<I', second, 132, zlib.crc32(second[:132]))
        data = blob([(1, first), (2, second)])
        data[16 + 44 * 136:16 + 45 * 136] = record()
        result = decoder.decode(data)
        self.assertEqual(result['pending']['kind'], 'pending_sleep')
        row = result['records'][1]
        self.assertEqual(row['independent_awake_us'], 7000000)
        self.assertEqual(row['awake_balance_us'], 0)
        self.assertTrue(row['raw_balance_valid'])
        struct.pack_into('<Q', second, 64, 3000300000 - 433000000)
        struct.pack_into('<I', second, 132, zlib.crc32(second[:132]))
        result = decoder.decode(blob([(1, first), (2, second)]))
        self.assertFalse(result['records'][1]['raw_balance_valid'])


if __name__ == '__main__':
    unittest.main()
