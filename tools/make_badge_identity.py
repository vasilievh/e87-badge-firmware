#!/usr/bin/env python3
"""Encode an optional private BLE name record for flash sector 0x30B000.

Keep the output outside the source tree; pass the name at runtime. This does not
flash a device or change Git. Names are printable ASCII, 1..20 bytes. Mouse mode
appends ' Mouse'; both advertisements still fit the 31-byte BLE response limit.
"""
import argparse
import binascii
from pathlib import Path


def encode(name):
    raw = name.encode("ascii")
    if not 1 <= len(raw) <= 20 or any(c < 32 or c > 126 for c in raw):
        raise ValueError("Name must contain 1..20 printable ASCII bytes")
    prefix = bytes((1, len(raw)))
    crc = binascii.crc_hqx(prefix + raw, 0xffff)
    return b"BID1" + prefix + crc.to_bytes(2, "little") + raw.ljust(24, b"\0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        args.output.write_bytes(encode(args.name))
    except (UnicodeError, ValueError) as error:
        parser.error(str(error))
    print(f"Wrote {args.output}: 32-byte device-local identity record")
