# Device-local font pack (system overlay)

Status: host tests, native loader tests and a clean JieLi SDK build pass. Confirmed
on hardware on 2026-11-09: the default pack was provisioned at 0x310000 over BLE and
Display titles and v2 card text render from it (owner check on the physical screen).
Historical first font-pack build: source `341c226` (built as `5df908a` before a message-only history rewrite),
[Actions run 42542516507](https://github.com/vasilievh/e87-badge-firmware/actions/runs/42542516507).
That application was 936,000 bytes (950,048 before removing the built-in 36px font),
leaving 59,616 bytes in the fixed 995,616-byte code slot. The later confirmed
Display v2 build `49f91a4` is 936,544 bytes with 59,072 bytes remaining;
see [current build status](ble-display.md).
The later [catalogue A/B changes](resources.md) have not yet had a vendor build
or physical-device test; these historical sizes do not include them.

The font pack is data in flash, like the wallpaper. The firmware reads one glyph
at a time; the pack is not compiled into the application. The built-in 36px
Montserrat (~14 KB of code, ASCII 32..90 only) is removed by the system overlay. The built-in 14px font
stays for the menu, Update and every fallback, so a missing or damaged pack never
blocks firmware installation.

## Provision

Open **Settings -> Update** on firmware with the system overlay, then:

```sh
python -m pip install pillow fonttools
python tools/make_badge_fonts.py /private/fonts.bin
python -m qix_ble.resources ADDRESS upload fonts /private/fonts.bin --reboot
```

The checked uploader requires catalogue firmware. With earlier font-pack
firmware, use `qix rawflash ADDRESS /private/fonts.bin --addr 0x310000 --verify --reboot`.
An existing unregistered BFN1 pack is still accepted with its original CRC16;
re-uploading registers a CRC32 of its entire record, including its header.

Default: Montserrat Medium ([SIL OFL 1.1](../tools/fonts/OFL.txt)) at 14, 20, 24 and 36 px,
4 bpp, 179 characters: printable ASCII, `ÄÖÜäöüß`, Russian `А-я Ёё` and `«»°–—“”„…€№`.
That pack is about 88 KB. `--sizes 14,24,36,48` (about 150 KB) also fits.
`--font` accepts another TrueType/OpenType file; the generator refuses fonts that
lack any character. An ordinary app-only OTA preserves the pack.
For Unicode in Display v2, include 14px and at least one of 20/24/36px. A valid
pack without those sizes does not enable Unicode. Reboot after provisioning so
the firmware reloads and validates the pack.

## Flash and RAM

Address ranges below exclude their end address.

| Range | Use |
| --- | --- |
| `0x2A0000..0x2E0000` | full-resolution wallpaper |
| `0x300000` / `0x30B000` | legacy wallpaper / device identity |
| `0x310000..0x33E000` | **font pack**, at most 188,416 bytes |
| `0x33E000` | VIRFAT (not used) |

RAM: one 1,024-byte glyph buffer (the largest 48px glyph is 1,008 bytes), the range
and size tables and one cached glyph record, about 1.4 KB in total. No heap use.

## Use in the firmware

`badge_fonts_init()` runs once while the UI is built. It reads the 16-byte header,
checks the CRC16 of bytes 16 through the end (the header is excluded), then
validates ranges and sizes before publishing any font. Rejected: wrong
magic/version, CRC mismatch, more than 4 sizes,
sizes outside 8..48 px or out of order, bpp other than 4, overlapping or unsorted
ranges, a first range other than ASCII 32..126, and any table outside the record.
Every glyph lookup bounds its bitmap again, because a CRC is not an authorization.

The Display title picks the largest available pack size from 36, 24 and 20px
whose text fits the 240px label on one line, otherwise the smallest available
of those sizes. If none is available it uses built-in 14px. A stored 48px size
is not selected for Display titles. Body and footer use the pack's 14px font
when present, with built-in 14px as fallback. Display v1 still accepts ASCII only;
Display v2 accepts UTF-8 characters covered by the pack when the required sizes
are present. The menu and Update screen keep the built-in font.

## Format (BFN1, little-endian)

| Part | Layout |
| --- | --- |
| Header, 16 B | `BFN1`, version 1, size count, range count, 0, total size u32, CRC16-CCITT (seed 0xFFFF) of bytes 16.. u16, glyph count u16 |
| Ranges, 4 B each | first codepoint u16, count u16 |
| Sizes, 12 B each | px, line height, baseline from bottom, bpp=4, glyph table offset u32, bitmap offset u32 |
| Glyphs, 8 B each | bitmap offset u24, advance, box width, box height, x offset i8, y offset i8 |
| Bitmaps | 4 bpp, high nibble first, rows packed without padding (LVGL's plain format) |

Kerning and compression are omitted: they would cost code, and flash space is ample.

## Tests

```sh
python -m pip install -e "tools/qix-ble[dev]" pillow fonttools
python -m pytest -q tests/test_font_pack.py tests/test_system_overlay.py
python scripts/test-display-native.py
```

The pack tests round-trip glyph pixels against Pillow exactly. The native harness
runs the real loader on a generated pack in fake flash: every glyph's metrics and
bitmap, title fitting, failed reads, truncation, rule violations, 3,000 random byte
flips (all rejected) and 3,000 adversarial edits with a recomputed CRC, with every
read asserted inside the record and every bitmap inside the buffer.
