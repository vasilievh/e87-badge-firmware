"""Read-only safety diagnostics for custom E87 firmware (badge_safety.h).

Open Bluetooth Display or Update (without starting an update), then run:
    python -m qix_ble.safety ADDRESS
Reports how many display TE interrupts fired (in total and during the last
panel sleep) and how far the UI loop has advanced. Nothing is written to the
badge: no bootstrap, pairing, flash, reset, OTA or display commands are sent.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

from qix_ble.errors import QixError

CMD, REPLY = 0x97, 0x98
ASLEEP, LAST_VALID, EXEMPT = 1, 2, 4


class SafetyError(RuntimeError):
    pass


class SafetyClient:
    def __init__(self, transport, timeout=5):
        self.transport, self.timeout = transport, timeout

    def page(self, page):
        if page not in range(3):
            raise ValueError("safety page must be 0..2")
        request = b"BS" + bytes((1, page))
        deadline = time.monotonic() + self.timeout
        frame = self.transport.send_command(CMD, request, flags=2, expect_cmd=REPLY,
                                            timeout=self.timeout, response=True)
        while True:
            payload = frame.payload
            if frame.cmd == REPLY and payload[:4] == request:
                if len(payload) != 14:
                    raise SafetyError("invalid safety response length")
                return payload[4:]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SafetyError("no matching safety response")
            frame = self.transport.wait_for_cmd(REPLY, timeout=remaining)

    def status(self):
        te_total, ui_loops, flags, stalled_s = struct.unpack("<IIBB", self.page(0))
        last_te, last_s, reserved1 = struct.unpack("<IIH", self.page(1))
        now_te, now_s, reserved2 = struct.unpack("<IIH", self.page(2))
        if flags & ~7 or reserved1 or reserved2:
            raise SafetyError("invalid safety response fields")

        def sleep(te, seconds):
            return {"seconds": seconds, "te_interrupts": te,
                    "te_per_second": round(te / seconds, 1) if seconds else None}

        return {
            "version": 1,
            "te_interrupts_total": te_total,
            "ui_loops": ui_loops,
            "watchdog_exempt": bool(flags & EXEMPT),
            "watchdog_stalled_s": stalled_s,
            "last_sleep": sleep(last_te, last_s) if flags & LAST_VALID else None,
            "current_sleep": sleep(now_te, now_s) if flags & ASLEEP else None,
        }


def main(argv=None):
    from qix_ble.transport import QixTransport

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="badge BLE address")
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    # The shared transport can pair when an OTA workflow sets this toggle.
    if os.environ.get("QIX_PAIR", "0") != "0":
        parser.error("read-only safety diagnostics require QIX_PAIR unset or 0")
    try:
        with QixTransport(args.address) as transport:
            print(json.dumps(SafetyClient(transport, args.timeout).status(), indent=2))
    except (QixError, SafetyError) as error:
        print(f"Safety diagnostics failed: {error}. Requires firmware with badge_safety.h "
              "and Bluetooth Display or Update open on the badge.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
