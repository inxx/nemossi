#!/usr/bin/env python3
"""Offline checks for a Muse app-only candidate. No device access or secrets output."""
import argparse
import functools
import hashlib
import json
import operator
from pathlib import Path
import struct

APP_OFFSET, APP_SIZE = 0x10000, 0x330000
PARTITIONS = [
    ("nvs", 1, 2, 0x9000, 0x5000),
    ("otadata", 1, 0, 0xe000, 0x2000),
    ("app0", 0, 16, APP_OFFSET, APP_SIZE),
    ("app1", 0, 17, 0x340000, APP_SIZE),
    ("spiffs", 1, 130, 0x670000, 0x180000),
    ("coredump", 1, 3, 0x7f0000, 0x10000),
]
FORBIDDEN = (
    "HOMEHUB_NVS_ENCRYPTION", "NVS_ENCRYPTION", "HOMEHUB_PAIRING_EFUSE_AUTH",
    "SECURE_BOOT", "SECURE_FLASH_ENC_ENABLED", "SECURE_SIGNED_APPS_NO_SECURE_BOOT",
    "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT", "SECURE_BOOT_BUILD_SIGNED_BINARIES",
    "HOMEHUB_OTA_ENABLED", "HOMEHUB_TUNNEL", "ESP_PHY_INIT_DATA_IN_PARTITION",
    "BOOTLOADER_APP_ROLLBACK_ENABLE", "MUSE_ENABLED",
    "HOMEHUB_SUPPORT_BUG_REPORT", "HOMEHUB_DEV_BUILD",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_config(text):
    values = dict(line.split("=", 1) for line in text.splitlines() if line.startswith("CONFIG_") and "=" in line)
    for name in FORBIDDEN:
        require(values.get("CONFIG_" + name, "n") == "n", "unsafe generated option: " + name)
    expected = {"PARTITION_TABLE_OFFSET": "0x8000", "MMU_PAGE_SIZE": "0x10000",
                "ESPTOOLPY_FLASHSIZE": '"16MB"', "ESPTOOLPY_FLASHMODE_DIO": "y",
                "ESPTOOLPY_FLASHFREQ_80M": "y", "HOMEHUB_LED_BACKEND_NEMOSSI": "y",
                "HOMEHUB_VOICE": "y", "SPIRAM": "y", "SPIRAM_MODE_OCT": "y",
                "NEMOSSI_TOUCH_ENABLED": "y"}
    for name, wanted in expected.items():
        require(values.get("CONFIG_" + name) == wanted, "generated option mismatch: " + name)
    return {"security_options_off": list(FORBIDDEN),
            "sdk_token_configured": values.get("CONFIG_GADGET_SDK_TOKEN", '""') != '""',
            "local_tts_configured": all(values.get("CONFIG_" + name, '""') != '""'
                                         for name in ("NEMOSSI_TTS_URL", "NEMOSSI_TTS_TOKEN"))}


def check_table(data):
    require(len(data) == 3072, "unexpected generated partition table length")
    entries = []
    for index in range(0, len(data), 32):
        block = data[index:index + 32]
        if block[:2] == b"\xeb\xeb":
            require(block[2:16] == b"\xff" * 14, "invalid partition MD5 record")
            require(hashlib.md5(data[:index]).digest() == block[16:], "partition MD5 mismatch")
            require(data[index + 32:] == b"\xff" * (len(data) - index - 32), "extra partition records")
            break
        require(block[:2] == b"\xaa\x50", "partition entry magic mismatch")
        _, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", block)
        name = label.split(b"\0", 1)[0].decode("ascii")
        require(flags == 0, "unexpected partition encryption/flags")
        entries.append((name, kind, subtype, offset, size))
    else:
        raise ValueError("partition MD5 record missing")
    require(entries == PARTITIONS, "generated table does not match preserved board geometry")
    return {"matches_original_geometry": True, "md5_valid": True}


def check_image(data):
    require(24 <= len(data) <= APP_SIZE, "app does not fit original app0")
    require(data[0] == 0xe9 and 1 <= data[1] <= 16, "invalid ESP image header")
    require(data[2:4] == b"\x02\x4f", "image must match original DIO/80MHz/16MiB settings")
    chip, min_rev, min_full, max_full = struct.unpack_from("<HBHH", data, 12)
    require(chip == 9 and min_rev <= 2 and min_full <= 2 <= max_full, "image excludes ESP32-S3 revision 0.2")
    require(data[23] == 1, "embedded image SHA256 is required")
    at, checksum = 24, 0xef
    for _ in range(data[1]):
        require(at + 8 <= len(data), "truncated image segment header")
        _, size = struct.unpack_from("<II", data, at)
        at += 8
        require(at + size <= len(data), "truncated image segment")
        checksum = functools.reduce(operator.xor, data[at:at + size], checksum)
        at += size
    checksum_at = at | 15
    require(checksum_at + 33 == len(data), "unexpected image padding or signing trailer")
    require(data[at:checksum_at] == b"\0" * (checksum_at - at), "invalid image alignment padding")
    require(data[checksum_at] == checksum, "ESP image checksum mismatch")
    end = checksum_at + 1
    require(hashlib.sha256(data[:end]).digest() == data[end:], "embedded image SHA256 mismatch")
    erase_bytes = (len(data) + 4095) // 4096 * 4096
    require(erase_bytes <= APP_SIZE, "sector erase would exceed app0")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "chip_id": chip, "chip_revision_supported": "0.2", "checksum_valid": True,
            "embedded_sha256_valid": True, "flash_offset": APP_OFFSET,
            "sector_erase_bytes": erase_bytes, "sector_erase_end_exclusive": APP_OFFSET + erase_bytes,
            "original_app0_size": APP_SIZE, "free_bytes": APP_SIZE - len(data)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--sdkconfig", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = {"config": check_config(args.sdkconfig.read_text()),
                  "partition_table": check_table((args.build / "partition_table/partition-table.bin").read_bytes()),
                  "app": check_image((args.build / "muse-gadget.bin").read_bytes()),
                  "hardware_tested": False, "device_written": False,
                  "flash_action": "none; app-only candidate must be separately approved"}
    except (ValueError, OSError, UnicodeError, struct.error) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
