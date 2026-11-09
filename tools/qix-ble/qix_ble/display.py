"""Badge Display v1/v2 client. Only RAM display commands; no auth/reset/OTA/flash calls.

v2 sends UTF-8 (German letters, Cyrillic, ...) when the badge has a font pack; it
falls back to v1 (printable ASCII) on older firmware or without a pack.

Run: python -m qix_ble.display ADDRESS hello
     python -m qix_ble.display ADDRESS card --title "Привет" --body "Größe: 5 °C"
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import json
import struct
import sys
import time

from qix_ble.errors import QixError

CMD, REPLY, VERSION, VERSION2 = 0x90, 0x91, 1, 2
HELLO, CARD, CLEAR, STATUS = 1, 2, 3, 4
LIMITS = (48, 96, 64)        # v1 byte limits
LIMITS2 = (64, 192, 96)      # v2 byte limits
PACKET_MAX = {VERSION: 227, VERSION2: 19 + sum(LIMITS2)}
FLAG_UNICODE = 1
STATUS_BAD_VERSION, STATUS_BAD_VALUE = 2, 4
ERRORS = {1: "malformed packet", 2: "unsupported protocol version", 3: "unknown command",
          4: "unsupported text or value (character missing from the badge font pack?)",
          5: "open Bluetooth Display on the badge", 6: "device busy"}


class DisplayError(RuntimeError):
    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


def color(value):
    if isinstance(value, bool):
        raise ValueError("color must be RRGGBB or an integer")
    if isinstance(value, str):
        value = int(value.removeprefix("#"), 16)
    if not isinstance(value, int) or not 0 <= value <= 0xFFFFFF:
        raise ValueError("color must be between 000000 and FFFFFF")
    return value.to_bytes(3, "big")


@dataclass(frozen=True)
class Card:
    title: str = ""
    body: str = ""
    footer: str = ""
    background: str | int = "101820"
    foreground: str | int = "FFFFFF"
    accent: str | int = "00C8A0"
    progress: int | None = None

    def encode(self, limits=LIMITS, unicode=False):
        """Card body. Without `unicode` only printable ASCII is allowed (v1, or v2
        without a font pack). The badge makes the final per-character decision."""
        texts = []
        for index, (value, limit) in enumerate(zip((self.title, self.body, self.footer), limits)):
            if not isinstance(value, str):
                raise ValueError("card text fields must be strings")
            for char in value:
                code = ord(char)
                if char == "\n" and index == 1:
                    continue
                if code < 32 or code == 127 or code > 0xFFFF or 0xD800 <= code <= 0xDFFF:
                    raise ValueError(f"text field {index} contains an unsupported character U+{code:04X}")
                if code > 126 and not unicode:
                    raise ValueError("this badge accepts printable ASCII only "
                                     "(firmware without Display v2 or without a font pack)")
            encoded = value.encode("utf-8")
            if len(encoded) > limit:
                raise ValueError(f"text field {index} is {len(encoded)} bytes; the limit is {limit} "
                                 "(Cyrillic and umlauts take 2 bytes each)")
            texts.append(encoded)
        progress = 255 if self.progress is None else self.progress
        if isinstance(progress, bool) or not isinstance(progress, int) or (progress != 255 and not 0 <= progress <= 100):
            raise ValueError("progress must be 0..100 or None")
        return (color(self.background) + color(self.foreground) + color(self.accent)
                + bytes((progress, *(len(t) for t in texts))) + b"".join(texts))


class DisplayClient:
    def __init__(self, transport, timeout=5):
        self.transport, self.timeout, self.sequence = transport, timeout, 0
        self.capabilities = None

    @property
    def version(self):
        return self.capabilities["version"] if self.capabilities else VERSION2

    def request(self, op, body=b"", version=None):
        version = version or self.version
        self.sequence = (self.sequence + 1) & 0xFFFF
        seq = self.sequence
        payload = b"BD" + bytes((version, op)) + struct.pack("<H", seq) + body
        if len(payload) > PACKET_MAX[version]:
            raise ValueError(f"display packet exceeds the v{version} maximum")
        deadline = time.monotonic() + self.timeout
        frame = self.transport.send_command(CMD, payload, flags=6 if len(payload) > 14 else 2, expect_cmd=REPLY,
                                            timeout=self.timeout, response=True)
        while True:
            p = frame.payload
            # Firmware answers an unsupported version with a v1 header (status 2).
            if (len(p) >= 7 and p[:2] == b"BD" and p[2] in (VERSION, version) and p[3] == op | 0x80
                    and int.from_bytes(p[4:6], "little") == seq):
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DisplayError("no matching display acknowledgement")
            frame = self.transport.wait_for_cmd(REPLY, timeout=remaining)
        if p[6] != 0:
            raise DisplayError(ERRORS.get(p[6], f"device error {p[6]}"), status=p[6])
        expected = (14 if version == VERSION else 13) if op == HELLO else 9
        if p[2] != version or len(p) != expected:
            raise DisplayError("invalid display acknowledgement length")
        return p[7:]

    def hello(self):
        """Negotiate v2; fall back to v1 only when the firmware rejects the version."""
        try:
            title, body, footer, flags, font_id = struct.unpack("<BBBBH", self.request(HELLO, version=VERSION2))
        except DisplayError as error:
            if error.status != STATUS_BAD_VERSION:
                raise
        else:
            if (title, body, footer) != LIMITS2:
                raise DisplayError("unsupported display capabilities")
            unicode = bool(flags & FLAG_UNICODE)
            self.capabilities = {"version": VERSION2, "width": 360, "height": 360,
                                 "text_limits": [title, body, footer],
                                 "charset": "UTF-8 (font pack)" if unicode else "ASCII",
                                 "unicode": unicode, "font_pack": f"{font_id:04X}" if unicode else None}
            return self.capabilities
        width, height, title, body, footer = struct.unpack("<HHBBB", self.request(HELLO, version=VERSION))
        if (title, body, footer) != LIMITS or (width, height) != (360, 360):
            raise DisplayError("unsupported display capabilities")
        self.capabilities = {"version": VERSION, "width": width, "height": height,
                             "text_limits": [title, body, footer], "charset": "ASCII",
                             "unicode": False, "font_pack": None}
        return self.capabilities

    def show(self, card):
        card.encode(LIMITS2, unicode=True)  # Reject malformed input before any BLE command.
        if self.capabilities is None:
            self.hello()
        caps = self.capabilities
        encoded = card.encode(tuple(caps["text_limits"]), unicode=caps["unicode"])
        return int.from_bytes(self.request(CARD, encoded), "little")

    def clear(self):
        if self.capabilities is None:
            self.hello()
        return int.from_bytes(self.request(CLEAR), "little")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Show RAM-only cards on custom E87 Display firmware")
    parser.add_argument("address", help="badge BLE address; never embedded in source")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("hello", "clear", "status", "demo"):
        sub.add_parser(name)
    card = sub.add_parser("card")
    for name in ("title", "body", "footer"):
        card.add_argument(f"--{name}", default="")
    for name, default in (("background", "101820"), ("foreground", "FFFFFF"), ("accent", "00C8A0")):
        card.add_argument(f"--{name}", default=default)
    card.add_argument("--progress", type=int)
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):  # Windows consoles: print Cyrillic errors safely.
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")
    # Validate user input before opening the Bluetooth connection.
    try:
        content = Card(**{k: getattr(args, k) for k in Card.__dataclass_fields__}) if args.command == "card" else None
        if content: content.encode(LIMITS2, unicode=True)
        from qix_ble.transport import QixTransport
        with QixTransport(args.address) as transport:
            client = DisplayClient(transport)
            info = client.hello()  # Timeout on old firmware: never fall back to vendor writes.
            if args.command == "card":
                print(json.dumps({"revision": client.show(content)}))
            elif args.command == "clear":
                print(json.dumps({"revision": client.clear()}))
            elif args.command == "status":
                print(json.dumps({"revision": int.from_bytes(client.request(STATUS), "little")}))
            elif args.command == "demo":
                for i in range(0, 101, 10):
                    client.show(Card("HELLO", "Bluetooth Display\nReady for new apps", "PC -> BLE -> badge", progress=i))
                    time.sleep(0.5)
                if info["unicode"]:
                    client.show(Card("Привет!", "Größe: 5 °C — «ёлка»", "Display v2 / UTF-8", progress=100))
                print("Demo complete")
            else: print(json.dumps(info, ensure_ascii=False))
    except (ValueError, QixError, RuntimeError) as error:
        print(f"Display: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
