#!/usr/bin/env python3
"""Wrap an ESP-IDF application binary in a Zigbee OTA container and index."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56
FIELD_CONTROL = 0
STACK_VERSION = 0x0002
UPGRADE_IMAGE_TAG = 0x0000


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    parser.add_argument("--manufacturer", type=parse_int, default=0xFFF1)
    parser.add_argument("--image-type", type=parse_int, default=0x0001)
    parser.add_argument("--file-version", type=parse_int, default=0x00010000)
    parser.add_argument("--version-name", default="1.0.0")
    args = parser.parse_args()

    app = args.binary.read_bytes()
    header_string = f"Soil Sentinel {args.version_name}".encode("ascii")[:32]
    header_string = header_string.ljust(32, b"\0")
    total_size = HEADER_LENGTH + 6 + len(app)
    header = struct.pack(
        "<IHHHHHIH32sI",
        FILE_IDENTIFIER,
        HEADER_VERSION,
        HEADER_LENGTH,
        FIELD_CONTROL,
        args.manufacturer,
        args.image_type,
        args.file_version,
        STACK_VERSION,
        header_string,
        total_size,
    )
    assert len(header) == HEADER_LENGTH
    element = struct.pack("<HI", UPGRADE_IMAGE_TAG, len(app)) + app
    ota = header + element

    args.output_dir.mkdir(parents=True, exist_ok=True)
    filename = f"soil-sentinel-{args.version_name}.ota"
    ota_path = args.output_dir / filename
    ota_path.write_bytes(ota)
    checksum = hashlib.sha3_256(ota).hexdigest()
    index = {
        "firmwares": [
            {
                "file_version": args.file_version,
                "file_size": len(ota),
                "image_type": args.image_type,
                "manufacturer_id": args.manufacturer,
                "manufacturer_names": ["longhornx10"],
                "model_names": ["Soil Sentinel"],
                "checksum": f"sha3-256:{checksum}",
                "path": filename,
                "changelog": "Field-ready Soil Sentinel firmware bundle.",
            }
        ]
    }
    (args.output_dir / "index.json").write_text(
        json.dumps(index, indent=2) + "\n", encoding="utf-8"
    )
    print(ota_path)


if __name__ == "__main__":
    main()
