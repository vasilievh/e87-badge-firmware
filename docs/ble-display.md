# Bluetooth Display v1/v2 (E87 development branch)

Status: host tests, a clean JieLi SDK build and the first E87 hardware check
passed on 2026-11-08. The badge accepted the OTA image, booted, negotiated Display
v1 over BLE and acknowledged a card update (revision 1). The owner confirmed the
title, body and teal progress bar were visible on the physical screen. The
upstream prebuilt firmware does not implement Display commands.

The initial v1 hardware check covered one card from a Windows host. The later v2
check below also covered the demo's repeated updates and custom cards. Reconnects,
leaving/reopening Display, CLEAR, long-running operation and each existing tool
after the update still need dedicated hardware checks.

Display v2: build `49f91a4` (`468a76e` before a message-only history rewrite; [Actions run 42554935192](https://github.com/vasilievh/e87-badge-firmware/actions/runs/42554935192),
application 936,544 bytes) was flashed from Windows on 2026-11-09 and the owner
confirmed on the physical screen that, with the font pack installed, the v2 demo
runs and shows Cyrillic, German letters and punctuation (`Привет!`,
`Größe: 5 °C — «ёлка»`), and custom cards work. Error paths, long-running use and old-client
compatibility have only been tested on the host.

Latest hardware-confirmed Display v2 application size (build `49f91a4` above): 936,544 bytes in
the 995,616-byte fixed OTA code slot, leaving 59,072 bytes. Current cards are
RAM-only and do not consume file space. This is the raw application size, not the
packaged OTA image size: the fixed-geometry E87 `.ufw` remains 1,079,363 bytes.
The later [resource catalogue A/B](resources.md) changes are not included in that
application-size figure and still need a new vendor build and hardware checks.

Historical Display v1 reference build: source commit `6fc8828c163b1ab195a5d6b7a34846a3fae2dbef`,
[GitHub Actions run 42529018450](https://github.com/vasilievh/e87-badge-firmware/actions/runs/42529018450).
The image is 1,079,363 bytes, SHA256
`0147557ba8e523952c0f2b33e51e850caf547ddb4f74e035df78dc639cb3c843`.
That build's application size was 991,264 bytes in the same fixed OTA code slot
(4,352 bytes remaining). These historical sizes predate the font and asset
reductions and do not describe the current build.

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
32..90. With the system overlay and a valid [flash font pack](flash-fonts.md),
titles use 36/24/20px by fit; a missing pack falls back to built-in 14px. The
progress indicator uses basic objects, since upstream compiles out the LVGL bar widget.
Updates replace one card in RAM. No filesystem, gallery or flash writes occur.
The existing Mouse, Scanner, Spoofing, Settings and Update tools remain in the menu.

Version 1 accepts printable ASCII, plus newlines in the body. Version 2 accepts
UTF-8 when the [flash font pack](flash-fonts.md) is installed: every character must
exist in the pack (default: ASCII, German letters, Russian Cyrillic, common
punctuation), and body/footer then use the pack's 14px font. Unicode requires
14px plus at least one of 20/24/36px in the pack. Without those sizes, v2 accepts
ASCII only. Newlines are accepted only in the body, in both versions. Images,
drawing primitives and touch events are later extensions.
Display v2 is confirmed on hardware (see status above).

Display enables BLE while its screen is open; the default name is **EC-BADGE**
(the system overlay can override it with a device-local identity).
Button 1 returns to the menu and disconnects. With the optional
[system overlay](badge-system.md), USB leaves the current tool/BLE session running
and updates the menu battery status; the home screen stays reserved for wallpaper.
Without it, upstream's charge-screen behavior ends Display mode on USB.
Disconnecting a PC leaves the last
card visible; reopening Display resets it to the waiting card.
Display does not require bonding, link encryption or an application password.
Nearby clients can replace the card while this mode is open.

## Build

Linux x86-64, including WSL2, is required for the actual vendor compiler.
For the current Display v2 build with font-pack support, apply both overlays:

```sh
bash scripts/setup-badge-build.sh --skip-postbuild
python3 scripts/prepare-display.py
python3 scripts/prepare-badge-system.py
bash scripts/build-badge-fw.sh --board e87 --ota firmware-builds/e87-display-v1.ufw
```

Run setup once on a pristine SDK, then build repeatedly. Without the system
overlay, rerunning `prepare-display.py` refreshes the two Display headers. Once
the system overlay is installed, changed Display/system headers require a fresh
SDK checkout/import and reapplying both overlays in the order above; rerunning the
scripts only verifies the existing installation. `--clean` cleans build objects,
not overlay manifests or SDK sources.

The overlays are separate from upstream's large patch; exact anchors and SHA256
manifests reject stale/partially edited SDK sources. Use a separate clean checkout
rather than resetting an SDK containing your own work. Omitting the system overlay
builds Display with ASCII-only text and upstream USB/charge-screen behavior.

If you already downloaded the pinned vendor ZIP, import it instead of Git cloning:

```sh
python3 scripts/import-sdk.py /path/to/vendor-sdk.zip
bash scripts/setup-badge-build.sh --skip-postbuild
python3 scripts/prepare-display.py
python3 scripts/prepare-badge-system.py
bash scripts/build-badge-fw.sh --board e87 --clean --ota firmware-builds/e87-display-v1.ufw
```

The importer accepts only the verified archive above and an empty destination.
It initializes a local Git working tree so the upstream patch can be applied,
but does not synthesize or publish a vendor commit. All SDK changes stay local.

On Windows without WSL, use the **Badge Display** GitHub Actions workflow. A code
push to `dev` (or a `feat/*` / `fix/*` branch cut from it) in this fork, or a manual run, runs the host tests,
fetches the pinned SDK/toolchain and
builds an `e87-display-v1` artifact containing the OTA image and SHA256SUMS.
Markdown-only pushes are skipped. The artifact/image name retains `v1` for
compatibility; the current workflow applies both overlays and supports v1/v2.
The system overlay now also includes the resource catalogue and checked upload protocol.
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
Install the [font pack](flash-fonts.md#provision) separately from the Update screen
before using the Unicode example. `hello` reports the version, byte limits and
whether Unicode is enabled; v2 firmware without a usable pack stays on v2 with
ASCII text. The client does not transliterate unsupported text.

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
non-ASCII text accepted) and the font pack id (LE16: its CRC16, with CRC 0 mapped
to 1; 0 means Unicode is unavailable),
a 13-byte payload. Firmware that does not know a version answers with a v1 header
and status 2; the client then falls back to v1 and ASCII. Other successful responses add revision:LE16. Errors
also include a revision; status 1 malformed, 2 version, 3 command, 4 value,
5 Display inactive, 6 mailbox busy. CARD and CLEAR increment the revision, wrapping
at 65536; STATUS returns it without changing the card. CLEAR shows an empty black
card with the progress bar hidden, rather than restoring the waiting message.

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
python -m pip install -e "tools/qix-ble[dev]" pillow fonttools
python -m pytest -q tools/qix-ble/tests/test_display.py tests/test_display_overlay.py
python scripts/test-display-native.py
```

The native harness compiles the actual protocol and UI adapter with radio/LVGL
stubs. It tests all truncations, limits, malformed input, 20,000 fuzz cases,
mailbox/back-pressure, UI-thread-only rendering, ACKs, exit and object reuse.
It uses system cc/gcc/clang, or `pip install ziglang==0.13.0` as a Windows fallback.
This does not substitute for SDK compilation, LVGL heap measurements, or a real
round-screen/BLE/lifecycle test after flashing.
