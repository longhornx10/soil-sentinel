#!/usr/bin/env python3
"""Wrap an ESP-IDF application binary in a Zigbee OTA container and index."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56
FIELD_CONTROL = 0
STACK_VERSION = 0x0002
UPGRADE_IMAGE_TAG = 0x0000

PROJECT_VER_RE = re.compile(r'PROJECT_VER\s+"([^"]+)"')
PROJECT_CMAKE = Path(__file__).resolve().parent.parent / "CMakeLists.txt"


def parse_int(value: str) -> int:
    return int(value, 0)


def read_project_ver() -> str | None:
    """Parse PROJECT_VER "x.y.z" from ../CMakeLists.txt, or None if absent."""
    try:
        content = PROJECT_CMAKE.read_text(encoding="utf-8")
    except OSError:
        return None
    match = PROJECT_VER_RE.search(content)
    return match.group(1) if match else None


def file_version_from_project_ver(project_ver: str) -> int | None:
    """Pack an exact x.y.z project version into a Zigbee file version."""
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", project_ver)
    if match is None:
        return None
    major, minor, patch = (int(part) for part in match.groups())
    return (major << 16) | (minor << 8) | patch


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    parser.add_argument("--manufacturer", type=parse_int, default=0xFFF1)
    parser.add_argument("--image-type", type=parse_int, default=0x0001)
    parser.add_argument("--file-version", type=parse_int, default=None)
    parser.add_argument("--version-name", default=None)
    args = parser.parse_args()

    project_ver = read_project_ver()
    derived = file_version_from_project_ver(project_ver) if project_ver else None

    if args.file_version is None:
        if derived is None:
            parser.error("--file-version is required (PROJECT_VER is not x.y.z)")
        args.file_version = derived
    elif derived is not None and args.file_version != derived:
        parser.error(
            f"--file-version 0x{args.file_version:08X} does not match "
            f"PROJECT_VER {project_ver} (0x{derived:08X})"
        )

    if args.version_name is None:
        args.version_name = project_ver if project_ver else "1.0.0"

    if (
        re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.version_name) is None
        or args.version_name in (".", "..")
        or ".." in args.version_name
    ):
        parser.error(
            f"--version-name {args.version_name!r} is invalid: must be a single "
            "path component matching [A-Za-z0-9][A-Za-z0-9._-]* without '..'"
        )

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
