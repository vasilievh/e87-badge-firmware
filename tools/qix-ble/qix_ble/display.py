"""Badge Display v1 client. Only RAM display commands; no auth/reset/OTA/flash calls.

Run: python -m qix_ble.display ADDRESS hello
     python -m qix_ble.display ADDRESS card --title Hello --body "From the PC"
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import json
import struct
import sys
import time

from qix_ble.errors import QixError

CMD, REPLY, VERSION = 0x90, 0x91, 1
HELLO, CARD, CLEAR, STATUS = 1, 2, 3, 4
LIMITS = (48, 96, 64)
ERRORS = {1: "malformed packet", 2: "unsupported protocol version", 3: "unknown command",
          4: "unsupported text or value", 5: "open Bluetooth Display on the badge", 6: "device busy"}


class DisplayError(RuntimeError):
    pass


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

    def encode(self):
        texts = []
        for index, (value, limit) in enumerate(zip((self.title, self.body, self.footer), LIMITS)):
            if not isinstance(value, str):
                raise ValueError("card text fields must be strings")
            try:
                encoded = value.encode("ascii")
            except UnicodeEncodeError as error:
                raise ValueError("v1 font supports printable ASCII; Unicode fonts are a later extension") from error
            if len(encoded) > limit or any(not (32 <= b <= 126 or index == 1 and b == 10) for b in encoded):
                raise ValueError(f"text field {index} must contain at most {limit} printable ASCII bytes")
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

    def request(self, op, body=b""):
        self.sequence = (self.sequence + 1) & 0xFFFF
        seq = self.sequence
        payload = b"BD" + bytes((VERSION, op)) + struct.pack("<H", seq) + body
        if len(payload) > 227:
            raise ValueError("display packet exceeds v1 maximum")
        deadline = time.monotonic() + self.timeout
        frame = self.transport.send_command(CMD, payload, flags=6 if len(payload) > 14 else 2, expect_cmd=REPLY,
                                            timeout=self.timeout, response=True)
        while True:
            p = frame.payload
            if (len(p) >= 7 and p[:3] == b"BD\x01" and p[3] == op | 0x80
                    and int.from_bytes(p[4:6], "little") == seq):
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DisplayError("no matching display acknowledgement")
            frame = self.transport.wait_for_cmd(REPLY, timeout=remaining)
        if p[6] != 0:
            raise DisplayError(ERRORS.get(p[6], f"device error {p[6]}"))
        expected = 14 if op == HELLO else 9
        if len(p) != expected:
            raise DisplayError("invalid display acknowledgement length")
        return p[7:]

    def hello(self):
        width, height, title, body, footer = struct.unpack("<HHBBB", self.request(HELLO))
        if (title, body, footer) != LIMITS or (width, height) != (360, 360):
            raise DisplayError("unsupported display capabilities")
        self.capabilities = {"version": VERSION, "width": width, "height": height,
                             "text_limits": [title, body, footer], "charset": "ASCII"}
        return self.capabilities

    def show(self, card):
        encoded = card.encode()  # Validate before any BLE command.
        if self.capabilities is None:
            self.hello()
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
    # Validate user input before opening the Bluetooth connection.
    try:
        content = Card(**{k: getattr(args, k) for k in Card.__dataclass_fields__}) if args.command == "card" else None
        if content: content.encode()
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
                    client.show(Card("HELLO", "Bluetooth Display v1\nReady for new apps", "PC -> BLE -> badge", progress=i))
                    time.sleep(0.5)
                print("Demo complete")
            else: print(json.dumps(info))
    except (ValueError, QixError, RuntimeError) as error:
        print(f"Display: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
