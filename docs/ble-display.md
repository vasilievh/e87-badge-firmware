# Bluetooth Display v1 (E87 development branch)

Status: source implementation with host tests. Not yet compiled with the JieLi
toolchain or validated on the badge. The previously installed upstream firmware
does not implement these commands. Do not send display commands to stock firmware.

Build verification, 2026-11-08: host tests passed on Windows and GitHub's Linux
runner. The validated compiler downloaded and ran successfully after pinning its
immutable archive URL. SDK cloning then failed at Git ref discovery with HTTP
502 from the vendor's GitLab; the retry stalled and was cancelled. An exact-commit
mirror was not found. The workflow now probes the SDK server first and reports a
clear bounded error rather than waiting indefinitely. No new OTA image exists yet.

## Scope

The new **Bluetooth Display** menu item presents a 360x360 card with a title,
body, footer, background/text/accent colors and an optional 0..100 progress bar.
The three text limits are 48, 96 and 64 bytes. Long text is clipped with dots;
the byte limits are transport limits, not a guarantee every character fits.
Uppercase titles use the existing 36px font; lowercase titles fall back to 14px
because upstream's large font has only glyphs 32..90. The progress indicator uses
basic objects, since upstream compiles out the LVGL bar widget.
Updates replace one card in RAM. No filesystem, gallery or flash writes occur.
The existing Mouse, Scanner, Spoofing, Settings and Update tools remain in the menu.

Version 1 deliberately accepts printable ASCII, plus newlines in the body.
The embedded fonts do not yet promise German umlauts or Cyrillic. Unicode fonts,
images, drawing primitives and touch events are subsequent protocol extensions.

BLE is enabled only while Display is open; it advertises as **EC-BADGE**.
Button 1 returns to the menu and disconnects. Plugging in USB follows upstream's
charge-screen behavior and ends Display mode. Disconnecting a PC leaves the last
card visible; reopening Display resets it to the waiting card.
Display does not require bonding, link encryption or an application password.
Nearby clients can replace the card while this mode is open.

## Build

Linux x86-64, including WSL2, is required for the actual vendor compiler:

```sh
bash scripts/setup-badge-build.sh --skip-postbuild
python3 scripts/prepare-display.py
bash scripts/build-badge-fw.sh --board e87 --ota firmware-builds/e87-display-v1.ufw
```

Run setup once on a pristine SDK, then build repeatedly. After changing either
header in `firmware/display`, rerun `prepare-display.py` before building.
The overlay is separate from upstream's large patch; exact anchors and an SHA256
manifest reject stale/partially edited SDK sources. To start afresh, use a separate
clean checkout rather than resetting an SDK containing your own work.

On Windows without WSL: push `feat/ble-display` to this fork. Its **Badge Display**
GitHub Actions workflow runs the host tests, fetches the pinned SDK/toolchain and
builds an `e87-display-v1` artifact containing the OTA image and SHA256SUMS.
The workflow's actual vendor build still needs a successful first run; host tests
do not establish flash readiness. It never connects to or flashes a device.

Use only the packaged `firmware-builds/*.ufw` after reviewing build results.
The native SDK `update.ufw` is not a BLE OTA image. The packaging continues to use
upstream's OEM-uboot-preserving base. See `ota-howto.md` for installation.

## PC client

```sh
python -m pip install -e tools/qix-ble
python -m qix_ble.display ADDRESS hello
python -m qix_ble.display ADDRESS card --title "HELLO" --body "From the PC" --progress 65
python -m qix_ble.display ADDRESS demo
python -m qix_ble.display ADDRESS clear
```

Open **Bluetooth Display** on the new firmware first. ADDRESS is supplied at
runtime, never embedded in source. No Windows pairing is required for Display.
If a previous HID bond blocks discovery, remove the old device entry on the host.

Library usage:

```python
from qix_ble.display import Card, DisplayClient
from qix_ble.transport import QixTransport

with QixTransport(address) as transport:
    display = DisplayClient(transport)
    display.show(Card("BUILD", "Tests passed", "CI status", progress=100))
```

## Protocol

Existing Qix framing on FD02 write / FD01 notify is reused, without changing GATT
UUIDs. Request opcode **0x90**, response **0x91** are reserved by this fork and
prefixed with magic `BD` to distinguish them from vendor messages.

Requests: `BD | version:u8=1 | operation:u8 | sequence:LE16 | body`.
HELLO=1, CARD=2, CLEAR=3, STATUS=4. HELLO/CLEAR/STATUS have no body.
CARD body: background RGB24, foreground RGB24, accent RGB24, progress:u8 (255
hides it), title/body/footer byte lengths:u8 each, then the three ASCII strings.
Maximum payload 227 bytes, complete Qix frame 233 bytes. Writes use response=True.

Responses start `BD | version | operation OR 0x80 | echoed sequence:LE16 | status`.
Successful HELLO adds width:LE16, height:LE16 and the three byte limits (14-byte
payload / 20-byte frame). Other successful responses add revision:LE16. Errors
also include a revision; status 1 malformed, 2 version, 3 command, 4 value,
5 Display inactive, 6 mailbox busy. Revision wraps at 65536.

The host negotiates HELLO before updates, checks sequence/operation, and waits
for an application acknowledgement after the UI applies each card. It does not
automatically retry mutations after a timeout: a lost ACK has an uncertain result.
There is one pending request; no unbounded queues, dynamic packet allocations or
LVGL calls in the BLE callback. Leaving Display clears the mailbox. Other Qix
commands, including OTA/raw flash, are ignored while Display is open; use Update
for firmware installation. Unknown/old firmware is never given fallback commands.

## Host validation (does not flash hardware)

```sh
python -m pytest -q tools/qix-ble/tests/test_display.py tests/test_display_overlay.py
python scripts/test-display-native.py
```

The native harness compiles the actual protocol and UI adapter with radio/LVGL
stubs. It tests all truncations, limits, malformed input, 20,000 fuzz cases,
mailbox/back-pressure, UI-thread-only rendering, ACKs, exit and object reuse.
It uses system cc/gcc/clang, or `pip install ziglang==0.13.0` as a Windows fallback.
This does not substitute for SDK compilation, LVGL heap measurements, or a real
round-screen/BLE/lifecycle test after flashing.
