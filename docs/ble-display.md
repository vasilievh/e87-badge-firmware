# Bluetooth Display v1 (E87 development branch)

Status: host tests, a clean JieLi SDK build and the first E87 hardware check
passed on 2026-11-08. The badge accepted the OTA image, booted, negotiated Display
v1 over BLE and acknowledged a card update (revision 1). The owner confirmed the
title, body and teal progress bar were visible on the physical screen. The
upstream prebuilt firmware does not implement Display commands.

This hardware check covers one card from a Windows host. Repeated updates,
reconnects, leaving/reopening Display, CLEAR, long-running operation and the
existing tools after this update have not yet been checked on hardware.

Display v2: build `49f91a4` (`468a76e` before a message-only history rewrite; [Actions run 42554935192](https://github.com/vasilievh/jieli-ble-badge-research/actions/runs/42554935192),
application 936,544 bytes) was flashed from Windows on 2026-11-09 and the owner
confirmed on the physical screen that, with the font pack installed, the v2 demo
runs and shows Cyrillic, German letters and punctuation («Привет!», «Größe: 5 °C —
«ёлка»»), and custom cards work. Error paths, long-running use and old-client
compatibility have only been tested on the host.

Reference build: source commit `6fc8828c163b1ab195a5d6b7a34846a3fae2dbef`,
[GitHub Actions run 42529018450](https://github.com/vasilievh/jieli-ble-badge-research/actions/runs/42529018450).
The image is 1,079,363 bytes, SHA256
`0147557ba8e523952c0f2b33e51e850caf547ddb4f74e035df78dc639cb3c843`.
Application size: 991,264 bytes in the 995,616-byte fixed OTA code slot (4,352
bytes remaining). Larger features will require code/asset size reductions or
separate data storage. Current cards are RAM-only and do not consume file space.

Offline verification checks the Qix wrapper, UFW headers/list/flash CRCs, JLFS
headers and changed application/parent CRCs. Flash geometry and the opaque OEM
loader/configuration payloads are byte-identical to the known-working base.

The exact SDK ZIP is verified by SHA256
`bdad4eee49456f3d2d3abf1a763b14d8bb0b0fd6f1fde104d7504fc364350683`.
All 43 modified upstream files match the patch's Git blob preimages. The workflow
now downloads that archive directly from JieLi and checks its full SHA256 before
importing it. No locally downloaded SDK or personal files are uploaded to GitHub.

## Scope

The new **Bluetooth Display** menu item presents a 360x360 card with a title,
body, footer, background/text/accent colors and an optional 0..100 progress bar.
The text limits are 48/96/64 bytes in v1 and 64/192/96 bytes in v2. Long text is clipped with dots;
the byte limits are transport limits, not a guarantee every character fits.
Without the system overlay, uppercase titles use the existing 36px font and
lowercase titles fall back to 14px because upstream's large font has only glyphs
32..90. With it, titles use the [flash font pack](flash-fonts.md) (36/24/20px by fit). The progress indicator uses
basic objects, since upstream compiles out the LVGL bar widget.
Updates replace one card in RAM. No filesystem, gallery or flash writes occur.
The existing Mouse, Scanner, Spoofing, Settings and Update tools remain in the menu.

Version 1 accepts printable ASCII, plus newlines in the body. Version 2 accepts
UTF-8 when the [flash font pack](flash-fonts.md) is installed: every character must
exist in the pack (default: ASCII, German letters, Russian Cyrillic, common
punctuation), and body/footer then use the pack's 14px font. Without a pack, v2
accepts ASCII only. Images, drawing primitives and touch events are later extensions.
Display v2 is confirmed on hardware (see status above).

BLE is enabled only while Display is open; it advertises as **EC-BADGE**.
Button 1 returns to the menu and disconnects. With the optional
[system overlay](badge-system.md), USB leaves the current tool/BLE session running
and updates home/menu battery status. Without it, upstream's charge-screen behavior
ends Display mode on USB. Disconnecting a PC leaves the last
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

If you already downloaded the pinned vendor ZIP, import it instead of Git cloning:

```sh
python3 scripts/import-sdk.py /path/to/vendor-sdk.zip
bash scripts/setup-badge-build.sh --skip-postbuild
python3 scripts/prepare-display.py
bash scripts/build-badge-fw.sh --board e87 --clean --ota firmware-builds/e87-display-v1.ufw
```

The importer accepts only the verified archive above and an empty destination.
It initializes a local Git working tree so the upstream patch can be applied,
but does not synthesize or publish a vendor commit. All SDK changes stay local.

On Windows without WSL: push `feat/ble-display` to this fork. Its **Badge Display**
GitHub Actions workflow runs the host tests, fetches the pinned SDK/toolchain and
builds an `e87-display-v1` artifact containing the OTA image and SHA256SUMS.
The workflow never connects to or flashes a device. Its successful reference run
is linked above; each new image still needs offline checks and a hardware test.

Use only the packaged `firmware-builds/*.ufw` after reviewing build results.
The native SDK `update.ufw` is not a BLE OTA image. The packaging continues to use
upstream's OEM-uboot-preserving base. See `ota-howto.md` for installation.

## PC client

```sh
python -m pip install -e tools/qix-ble
python -m qix_ble.display ADDRESS hello
python -m qix_ble.display ADDRESS card --title "HELLO" --body "From the PC" --progress 65
python -m qix_ble.display ADDRESS card --title "Привет" --body "Größe: 5 °C"
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

Requests: `BD | version:u8 (1 or 2) | operation:u8 | sequence:LE16 | body`.
HELLO=1, CARD=2, CLEAR=3, STATUS=4. HELLO/CLEAR/STATUS have no body.
CARD body: background RGB24, foreground RGB24, accent RGB24, progress:u8 (255
hides it), title/body/footer byte lengths:u8 each, then the three strings.
v1: ASCII, maximum payload 227 bytes (233-byte Qix frame). v2: UTF-8 with 1..3-byte
sequences only (no overlong forms, surrogates or characters beyond U+FFFF), limits
64/192/96 bytes, maximum payload 371 bytes (377-byte frame, one WinRT write, below
the badge's 1,100-byte receive buffer). Writes use response=True.

Responses start `BD | version | operation OR 0x80 | echoed sequence:LE16 | status`.
Successful v1 HELLO adds width:LE16, height:LE16 and the three byte limits (14-byte
payload / 20-byte frame). Successful v2 HELLO adds the three v2 limits, flags (bit 0:
non-ASCII text accepted) and the font pack id (its CRC16; 0 without a usable pack),
a 13-byte payload. Firmware that does not know a version answers with a v1 header
and status 2; the client then falls back to v1 and ASCII. Other successful responses add revision:LE16. Errors
also include a revision; status 1 malformed, 2 version, 3 command, 4 value,
5 Display inactive, 6 mailbox busy. Revision wraps at 65536.

The host negotiates HELLO before updates, checks sequence/operation, and waits
for an application acknowledgement after the UI applies each card. The BLE task
validates a request, then decodes it directly into the free mailbox; the UI task
renders from the mailbox and only then frees it, so no card is copied onto either
task's stack. It does not
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
