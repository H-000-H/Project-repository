#!/usr/bin/env python3
"""Print firmware Flash/RAM usage with MCU limit percentages."""

from __future__ import annotations

import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size-tool", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--flash-bytes", type=int, default=294912)
    parser.add_argument("--ram-bytes", type=int, default=32768)
    args = parser.parse_args()

    result = subprocess.run(
        [args.size_tool, "--format=berkeley", args.elf],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        print("size output is empty", file=sys.stderr)
        return 1

    parts = lines[-1].split()
    if len(parts) < 3:
        print(f"failed to parse size output: {lines[-1]}", file=sys.stderr)
        return 1

    text, data, bss = int(parts[0]), int(parts[1]), int(parts[2])
    flash_used = text + data
    ram_used = data + bss

    def pct(used: int, total: int) -> float:
        return (used * 100.0 / total) if total else 0.0

    print(f"  FLASH : {flash_used:7d} / {args.flash_bytes} bytes  ({pct(flash_used, args.flash_bytes):5.1f}%)")
    print(f"  RAM   : {ram_used:7d} / {args.ram_bytes} bytes  ({pct(ram_used, args.ram_bytes):5.1f}%)")
    print("==================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
