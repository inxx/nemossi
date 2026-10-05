"""Offline candidate gate rejects coherent but incompatible build artifacts."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("check_muse_build", Path(__file__).resolve().parents[1] / "scripts/check_muse_build.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def table(entries=gate.PARTITIONS):
    records = b"".join(struct.pack("<HBBII16sI", 0x50aa, kind, subtype, offset, size, name.encode(), 0)
                       for name, kind, subtype, offset, size in entries)
    data = records + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(records).digest()
    return data + b"\xff" * (3072 - len(data))


def image(chip=9, minimum=0):
    header = bytearray(24)
    header[:4] = b"\xe9\x01\x02\x4f"
    struct.pack_into("<HBHH", header, 12, chip, 0, minimum, 99)
    header[23] = 1
    segment = b"\x01\x02\x03\x04"
    data = bytes(header) + struct.pack("<II", 0x3fc80000, len(segment)) + segment
    data += b"\0" * ((len(data) | 15) - len(data)) + bytes([0xef ^ 1 ^ 2 ^ 3 ^ 4])
    return data + hashlib.sha256(data).digest()


def config():
    return '\n'.join('CONFIG_' + key + '=' + value for key, value in {
        'PARTITION_TABLE_OFFSET': '0x8000', 'MMU_PAGE_SIZE': '0x10000',
        'ESPTOOLPY_FLASHSIZE': '"16MB"', 'ESPTOOLPY_FLASHMODE_DIO': 'y',
        'ESPTOOLPY_FLASHFREQ_80M': 'y', 'HOMEHUB_LED_BACKEND_NEMOSSI': 'y',
        'HOMEHUB_VOICE': 'y', 'SPIRAM': 'y', 'SPIRAM_MODE_OCT': 'y',
        'NEMOSSI_TOUCH_ENABLED': 'y',
    }.items())


class MuseBuildGate(unittest.TestCase):
    def test_preserved_layout_and_corrupt_md5(self):
        self.assertTrue(gate.check_table(table())['matches_original_geometry'])
        damaged = bytearray(table()); damaged[192 + 16] ^= 1
        with self.assertRaises(ValueError): gate.check_table(damaged)

    def test_valid_md5_does_not_allow_moved_app_or_added_phy(self):
        changed = list(gate.PARTITIONS)
        changed[2] = ('app0', 0, 16, 0x20000, gate.APP_SIZE)
        for entries in (changed, gate.PARTITIONS + [('phy_init', 1, 1, 0x800000, 0x1000)]):
            with self.assertRaises(ValueError): gate.check_table(table(entries))

    def test_every_unsafe_option_is_rejected(self):
        self.assertFalse(gate.check_config(config())['sdk_token_configured'])
        for option in gate.FORBIDDEN:
            with self.subTest(option=option), self.assertRaises(ValueError):
                gate.check_config(config() + '\nCONFIG_' + option + '=y')

    def test_stale_table_offset_is_rejected_without_exposing_token(self):
        with self.assertRaisesRegex(ValueError, 'PARTITION_TABLE_OFFSET'):
            gate.check_config(config().replace('0x8000', '0x10000') + '\nCONFIG_GADGET_SDK_TOKEN="test-private-value"')

    def test_image_digest_checksum_and_sector_bounds(self):
        result = gate.check_image(image())
        self.assertTrue(result['embedded_sha256_valid'])
        self.assertEqual(result['sector_erase_end_exclusive'], 0x11000)
        for at in (24 + 8, -1):
            changed = bytearray(image()); changed[at] ^= 1
            with self.assertRaises(ValueError): gate.check_image(changed)

    def test_valid_digest_cannot_allow_another_chip_or_revision(self):
        for data in (image(chip=5), image(minimum=3)):
            with self.assertRaises(ValueError): gate.check_image(data)

    def test_signing_trailer_and_oversized_app_are_rejected(self):
        for data in (image() + b'\xff' * 4096, b'\0' * (gate.APP_SIZE + 1)):
            with self.assertRaises(ValueError): gate.check_image(data)


if __name__ == '__main__':
    unittest.main()
