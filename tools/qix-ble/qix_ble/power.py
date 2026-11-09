"""Read-only power diagnostics for custom E87 firmware.

Open Bluetooth Display (or Update without starting an update), then run:
    python -m qix_ble.power ADDRESS
No bootstrap, pairing, flash, reset, OTA or display commands are sent.
The voltage/percent are cached SDK estimates; this does not measure current.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

from qix_ble.errors import QixError

CMD, REPLY = 0x92, 0x93


class PowerError(RuntimeError):
    pass


class PowerClient:
    def __init__(self, transport, timeout=5):
        self.transport, self.timeout = transport, timeout

    def page(self, page):
        if page not in range(4):
            raise ValueError("power page must be 0..3")
        request = b"BP" + bytes((1, page))
        deadline = time.monotonic() + self.timeout
        frame = self.transport.send_command(CMD, request, flags=2, expect_cmd=REPLY,
                                            timeout=self.timeout, response=True)
        while True:
            payload = frame.payload
            if frame.cmd == REPLY and payload[:4] == request:
                if len(payload) != 14:
                    raise PowerError("invalid power response length")
                return payload[4:]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PowerError("no matching power response")
            frame = self.transport.wait_for_cmd(REPLY, timeout=remaining)

    def status(self):
        # Pages are individually atomic snapshots. Their timestamps may differ
        # slightly; do not interpret them as a simultaneous current measurement.
        flags, percent, mv, uptime, lowpower, reserved = struct.unpack("<BBHIBB", self.page(0))
        before, after, before_mv, after_mv, seconds = struct.unpack("<BBHHI", self.page(1))
        locked_seconds, waits, reserved2 = struct.unpack("<IIH", self.page(2))
        sys_hz, idle_hz, unlock_result, lock_result = struct.unpack("<IIbb", self.page(3))
        if flags & ~31 or percent > 100 or before > 100 or after > 100 or reserved or reserved2:
            raise PowerError("invalid power response fields")
        return {
            "version": 1,
            "percent": percent, "battery_mv": mv, "uptime_s": uptime,
            "locked": bool(flags & 1), "usb_present": bool(flags & 2),
            "full_flag": bool(flags & 4), "ble_wanted": bool(flags & 8),
            "lowpower_config": lowpower, "sys_hz": sys_hz,
            "last_idle_hz": idle_hz, "clock_unlock_result": unlock_result,
            "clock_restore_result": lock_result,
            "locked_s": locked_seconds, "lock_waits_since_boot": waits,
            "last_lock": {"seconds": seconds, "before_percent": before,
                          "after_percent": after, "before_mv": before_mv,
                          "after_mv": after_mv} if flags & 16 else None,
        }


def main(argv=None):
    from qix_ble.transport import QixTransport

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="badge BLE address")
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    # The shared transport can explicitly pair when this environment toggle is
    # set by an OTA workflow. Refuse it here before opening a BLE connection.
    if os.environ.get("QIX_PAIR", "0") != "0":
        parser.error("read-only power diagnostics require QIX_PAIR unset or 0")
    try:
        # Unlike the general CLI, no OEM initialization/bootstrap dance.
        with QixTransport(args.address) as transport:
            print(json.dumps(PowerClient(transport, args.timeout).status(), indent=2))
    except (QixError, PowerError) as error:
        print(f"Power diagnostics failed: {error}. Requires the power-diagnostics firmware "
              "and Bluetooth Display or Update open on the badge.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
