"""E87 BRC1 resource catalogue and checked upload. No firmware OTA commands.

python -m qix_ble.resources ADDRESS list
python -m qix_ble.resources ADDRESS upload wallpaper /private/wallpaper.bin --reboot
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import struct
import zlib

TABLE_A, TABLE_B, SECTOR = 0x2E0000, 0x2E1000, 4096
MAGIC, COMMIT, MAX_ENTRIES = 0x31435242, 0x454E4F44, 3
COMMAND, REPLY = 0x94, 0x95
BEGIN, FINISH, ABORT, PROBE = 1, 2, 3, 4
KINDS = {"wallpaper": 1, "fonts": 2, "identity": 3}
DEFAULT_ADDRESSES = {1: 0x2A0000, 2: 0x310000, 3: 0x30B000}
ERRORS = {1: "malformed request/version", 2: "invalid resource or bounds",
          3: "another upload is active", 4: "flash I/O failure",
          5: "full resource CRC/header verification failed", 6: "no active upload",
          7: "open Settings -> Update on the badge"}


class ResourceError(RuntimeError):
    pass


@dataclass(frozen=True)
class Entry:
    id: int
    format: int
    address: int
    length: int
    crc: int
    reserved: int = 0

    def validate(self, *, unavailable=True):
        if self.id not in DEFAULT_ADDRESSES or self.format != 1 or self.reserved:
            raise ValueError("unsupported resource id/format/reserved fields")
        if not self.length:
            if unavailable and self.address == self.crc == 0:
                return
            raise ValueError("empty resource")
        if self.id == 1:
            start, end, maximum = (0x300000, 0x30B000, 41488) if self.address >= 0x300000 else (0x2A0000, 0x2E0000, 259216)
        elif self.id == 2:
            start, end, maximum = 0x310000, 0x33E000, 0x2E000
        else:
            start, end, maximum = 0x30B000, 0x30C000, 32
        if (self.address % SECTOR or not start <= self.address < end
                or not 0 < self.length <= min(maximum, end - self.address)
                or not 0 <= self.crc <= 0xFFFFFFFF):
            raise ValueError("resource exceeds its sector-aligned type window")

    def encode(self):
        self.validate()
        return struct.pack("<6I", *asdict(self).values())


def decode_catalog(data: bytes):
    if len(data) < 32:
        raise ValueError("short catalogue")
    magic, generation, count, length, crc, r1, r2, commit = struct.unpack_from("<8I", data)
    if (magic != MAGIC or count > MAX_ENTRIES or length != 32 + count * 24
            or len(data) != length or r1 or r2 or commit != COMMIT):
        raise ValueError("invalid/uncommitted catalogue header")
    if zlib.crc32(data[:16] + data[20:28] + data[32:]) != crc:
        raise ValueError("catalogue CRC mismatch")
    entries = [Entry(*struct.unpack_from("<6I", data, 32 + i * 24)) for i in range(count)]
    for entry in entries:
        entry.validate()
    if [entry.id for entry in entries] != sorted({entry.id for entry in entries}):
        raise ValueError("duplicate or unsorted resource ids")
    return generation, entries


def make_entry(kind: str, data: bytes, address=None):
    rid = KINDS[kind]
    address = DEFAULT_ADDRESSES[rid] if address is None else address
    entry = Entry(rid, 1, address, len(data), zlib.crc32(data))
    entry.validate(unavailable=False)
    if rid == 1:
        if len(data) < 16:
            raise ValueError("short LKMG wallpaper")
        magic, width, height, size, _ = struct.unpack_from("<4sHHII", data)
        limit = 144 if address >= 0x300000 else 360
        valid = (magic == b"LKMG" and 16 <= width <= limit and 16 <= height <= limit
                 and size == width * height * 2 and len(data) == 16 + size)
    elif rid == 2:
        valid = len(data) >= 16 and data[:5] == b"BFN1\x01" and struct.unpack_from("<I", data, 8)[0] == len(data)
    else:
        import binascii
        valid = (len(data) == 32 and data[:5] == b"BID1\x01" and 1 <= data[5] <= 20
                 and all(32 <= b <= 126 for b in data[8:8+data[5]])
                 and binascii.crc_hqx(data[4:6] + data[8:8+data[5]], 0xFFFF) == int.from_bytes(data[6:8], "little"))
    if not valid:
        raise ValueError("resource blob has an invalid header/length")
    return entry


class ResourceClient:
    def __init__(self, transport, timeout=15):
        from qix_ble.stager import StagerClient
        self.transport = transport
        self.raw = StagerClient(transport, timeout=timeout)
        self.timeout, self.sequence = timeout, 0

    def request(self, operation, body=b""):
        self.sequence = (self.sequence + 1) & 0xFFFF
        header = b"BR\x01" + bytes((operation,)) + struct.pack("<H", self.sequence)
        response = self.transport.send_command(
            COMMAND, header + body, flags=6 if len(body) else 2,
            expect_cmd=REPLY, timeout=self.timeout, response=True,
        )
        expected = b"BR\x01" + bytes((operation | 0x80,)) + struct.pack("<H", self.sequence)
        if len(response.payload) != 11 or response.payload[:6] != expected:
            raise ResourceError("invalid resource acknowledgement; operation outcome may be uncertain")
        if response.payload[6]:
            raise ResourceError(ERRORS.get(response.payload[6], "unknown resource error"))
        return int.from_bytes(response.payload[7:], "little")

    def _read(self, address, length):
        result = bytearray()
        while len(result) < length:
            n = min(13, length - len(result))  # Qix header 6 + status 1 + data <= ATT 20.
            piece = self.raw.read(address + len(result), n)
            if len(piece) != n:
                raise ResourceError("short raw read")
            result.extend(piece)
        return bytes(result)

    def catalog(self):
        self.request(PROBE)  # Unknown/older firmware must fail before any mutation.
        candidates = []
        for address in (TABLE_A, TABLE_B):
            header = self._read(address, 32)
            magic, _, count, length = struct.unpack_from("<4I", header)
            if magic != MAGIC or count > MAX_ENTRIES or length != 32 + count * 24:
                continue
            data = header + self._read(address + 32, length - 32)
            try:
                generation, entries = decode_catalog(data)
            except ValueError:
                continue
            candidates.append((generation, address, entries))
        if not candidates:
            return {"generation": 0, "table": None, "entries": []}
        selected = candidates[0]
        for candidate in candidates[1:]:
            if 0 < ((candidate[0] - selected[0]) & 0xFFFFFFFF) < 0x80000000:
                selected = candidate
        return {"generation": selected[0], "table": selected[1],
                "entries": [asdict(entry) for entry in selected[2]]}

    def upload(self, kind, data, *, address=None, reboot=False):
        entry = make_entry(kind, data, address)
        self.request(PROBE)
        begun = False
        try:
            # A BEGIN whose reply is lost may already have invalidated the old
            # resource; never retry a mutation automatically.
            self.request(BEGIN, entry.encode())
            begun = True
            self.raw.flash_region(entry.address, data, verify=False)
            generation = self.request(FINISH)  # Device checks every byte, then commits A/B.
            begun = False
        finally:
            if begun:
                try:
                    self.request(ABORT)
                except Exception:
                    pass
        if reboot:
            self.raw.reboot()
        return generation


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="badge BLE address")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    upload = commands.add_parser("upload")
    upload.add_argument("kind", choices=KINDS)
    upload.add_argument("file", type=Path)
    upload.add_argument("--addr", type=lambda value: int(value, 0))
    upload.add_argument("--reboot", action="store_true")
    args = parser.parse_args(argv)
    from qix_ble.errors import QixError
    try:
        data = args.file.read_bytes() if args.command == "upload" else None
        if data is not None:
            make_entry(args.kind, data, args.addr)  # Validate before opening BLE.
        from qix_ble.transport import QixTransport
        with QixTransport(args.address) as transport:
            client = ResourceClient(transport)
            result = (client.catalog() if data is None else
                      {"committed_generation": client.upload(args.kind, data, address=args.addr, reboot=args.reboot)})
        print(json.dumps(result))
    except (OSError, ValueError, RuntimeError, QixError) as error:
        parser.exit(1, f"Resources: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
