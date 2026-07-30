#!/usr/bin/env python3
"""Verify a PE binary's IMAGE_FILE_HEADER.Machine field matches an expected value."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

IMAGE_FILE_MACHINE_I386 = 0x14C
IMAGE_FILE_MACHINE_AMD64 = 0x8664


def validated_pe_path(value: str) -> Path:
    requested = Path(value)
    if requested.is_absolute() or ".." in requested.parts:
        raise ValueError("PE path must name a file beneath the current directory")

    base_directory = Path.cwd().resolve()
    path = (base_directory / requested).resolve()
    if not path.is_relative_to(base_directory) or not path.is_file():
        raise ValueError("PE path must name a file beneath the current directory")
    return path


def read_machine(path: Path) -> int:
    with path.open("rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE/DOS file (bad MZ signature)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError(f"{path}: no PE signature at offset {e_lfanew}")
    (machine,) = struct.unpack_from("<H", data, e_lfanew + 4)
    return machine


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <path-to-pe-file> <expected-machine-hex>", file=sys.stderr)
        return 2
    path_str, expected_str = sys.argv[1], sys.argv[2]
    try:
        path = validated_pe_path(path_str)
        expected = int(expected_str, 16)
        actual = read_machine(path)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    if actual != expected:
        print(
            f"{path}: expected machine 0x{expected:X}, got 0x{actual:X}",
            file=sys.stderr,
        )
        return 1
    print(f"{path}: machine 0x{actual:X} as expected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
