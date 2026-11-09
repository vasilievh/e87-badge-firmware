#!/usr/bin/env python3
"""Build the device-local BFN1 font pack for flash 0x310000 (E87 system overlay).

The firmware reads one glyph at a time from this record; nothing is compiled into
the application. The default pack is Montserrat Medium (SIL OFL, tools/fonts) at
14/20/24/36 px with ASCII, German letters, Russian Cyrillic and common punctuation.

  python tools/make_badge_fonts.py /private/fonts.bin
  qix rawflash ADDRESS /private/fonts.bin --addr 0x310000 --verify --reboot

Requires Pillow and fontTools. This does not connect to or flash a device.

Layout, all little-endian (see firmware/display/badge_fonts.h):
  header 16 B : "BFN1", version=1, font count, range count, 0,
                total size u32, CRC16-CCITT(seed 0xFFFF) of bytes 16.. u16, glyph count u16
  ranges      : start u16, length u16; sorted, disjoint; the first is ASCII 32..126
  fonts 12 B  : px, line height, baseline (from bottom), bpp=4, glyph table u32, bitmaps u32
  glyphs 8 B  : bitmap offset u24 (from the font's bitmaps), advance, box w, box h,
                x offset i8, y offset i8 (bottom of box above the baseline)
  bitmaps     : 4 bpp, MSB nibble first, rows packed without padding (LVGL format)
"""
from __future__ import annotations
import argparse
import binascii
from pathlib import Path
import struct

SLOT = 0x310000
SLOT_SIZE = 0x2E000  # Ends at VIRFAT (0x33E000).
DEFAULT_FONT = Path(__file__).resolve().parent / "fonts" / "Montserrat-Medium.ttf"
DEFAULT_SIZES = (14, 20, 24, 36)
MIN_PX, MAX_PX = 8, 48
MAX_FONTS, MAX_RANGES, MAX_GLYPHS = 4, 32, 512
BITMAP_MAX = 1024  # Firmware glyph buffer.
BPP = 4

CHARSET = (
    [chr(c) for c in range(0x20, 0x7F)]
    + list("ÄÖÜäöüß")
    + [chr(c) for c in range(0x410, 0x450)] + ["Ё", "ё"]
    + list("«»°–—“”„…€№")
)


def ranges_for(codepoints):
    result = []
    for cp in sorted(set(codepoints)):
        if result and result[-1][0] + result[-1][1] == cp:
            result[-1][1] += 1
        else:
            result.append([cp, 1])
    return [tuple(r) for r in result]


def pack_4bpp(values):
    out = bytearray((len(values) + 1) // 2)
    for i, v in enumerate(values):
        out[i // 2] |= v << 4 if i % 2 == 0 else v
    return bytes(out)


def render(font_path, px, codepoints):
    """Return (line_height, base_line, glyphs) where glyphs are (adv, w, h, x, y, bitmap)."""
    from PIL import ImageFont
    font = ImageFont.truetype(str(font_path), px)
    glyphs = []
    top, bottom = 0, 0
    for cp in codepoints:
        mask, (dx, dy) = font.getmask2(chr(cp), mode="L", anchor="ls")
        box = mask.getbbox()
        adv = round(font.getlength(chr(cp)))
        if not box:
            glyphs.append((adv, 0, 0, 0, 0, b""))
            continue
        x0, y0, x1, y1 = box
        w, h = x1 - x0, y1 - y0
        values = [round(mask.getpixel((x, y)) * 15 / 255) for y in range(y0, y1) for x in range(x0, x1)]
        ofs_x, ofs_y = dx + x0, -(dy + y1)
        top, bottom = max(top, ofs_y + h), min(bottom, ofs_y)
        if not (0 <= adv <= 255 and w <= 255 and h <= 255 and -128 <= ofs_x <= 127 and -128 <= ofs_y <= 127):
            raise ValueError(f"glyph U+{cp:04X} at {px}px does not fit the record fields")
        bitmap = pack_4bpp(values)
        if len(bitmap) > BITMAP_MAX:
            raise ValueError(f"glyph U+{cp:04X} at {px}px needs {len(bitmap)} > {BITMAP_MAX} bytes")
        glyphs.append((adv, w, h, ofs_x, ofs_y, bitmap))
    line_height, base_line = top - bottom, -bottom
    if not 0 <= base_line < line_height <= 255:
        raise ValueError(f"invalid line metrics at {px}px")
    return line_height, base_line, glyphs


def check_coverage(font_path, codepoints):
    from fontTools.ttLib import TTFont
    cmap = TTFont(str(font_path)).getBestCmap()
    missing = [cp for cp in codepoints if cp not in cmap]
    if missing:
        raise ValueError("font lacks " + ", ".join(f"U+{cp:04X}" for cp in missing))


def build(font_path=DEFAULT_FONT, sizes=DEFAULT_SIZES, charset=CHARSET):
    sizes = sorted(sizes)
    if not 1 <= len(sizes) <= MAX_FONTS or len(set(sizes)) != len(sizes):
        raise ValueError(f"choose 1..{MAX_FONTS} distinct sizes")
    if any(not MIN_PX <= px <= MAX_PX for px in sizes):
        raise ValueError(f"sizes must be {MIN_PX}..{MAX_PX} px")
    ranges = ranges_for(ord(c) for c in charset)
    codepoints = [s + i for s, n in ranges for i in range(n)]
    if not ranges or ranges[0] != (32, 95):
        raise ValueError("the pack must contain the complete printable ASCII range")
    if len(ranges) > MAX_RANGES or len(codepoints) > MAX_GLYPHS or codepoints[-1] > 0xFFFF:
        raise ValueError("character set exceeds firmware limits")
    check_coverage(font_path, codepoints)
    rendered = [(px, *render(font_path, px, codepoints)) for px in sizes]
    offset = 16 + 4 * len(ranges) + 12 * len(sizes)
    fonts, payload = b"", b""
    for px, line_height, base_line, glyphs in rendered:
        # Each font's 8-byte glyph table is immediately followed by its bitmaps.
        table_at = offset + len(payload)
        bitmap_at = table_at + 8 * len(glyphs)
        table, data = bytearray(), bytearray()
        for adv, w, h, x, y, bitmap in glyphs:
            table += len(data).to_bytes(3, "little") + struct.pack("<BBBbb", adv, w, h, x, y)
            data += bitmap
        fonts += struct.pack("<BBBBII", px, line_height, base_line, BPP, table_at, bitmap_at)
        payload += table + data
    body = b"".join(struct.pack("<HH", s, n) for s, n in ranges) + fonts + payload
    total = 16 + len(body)
    if total > SLOT_SIZE:
        raise ValueError(f"pack is {total} bytes; the flash slot holds {SLOT_SIZE}")
    crc = binascii.crc_hqx(body, 0xFFFF)
    header = b"BFN1" + bytes((1, len(sizes), len(ranges), 0)) + struct.pack("<IHH", total, crc, len(codepoints))
    return header + body


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="keep it outside the repository")
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT, help="TrueType/OpenType file")
    parser.add_argument("--sizes", default=",".join(map(str, DEFAULT_SIZES)),
                        help=f"comma-separated px sizes ({MIN_PX}..{MAX_PX}, at most {MAX_FONTS})")
    args = parser.parse_args(argv)
    try:
        sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
        blob = build(args.font, sizes)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_bytes(blob)
    print(f"Wrote {args.output}: {len(blob)} bytes, {len(sizes)} sizes, {len(CHARSET)} characters")
    print(f"push: qix rawflash ADDRESS {args.output} --addr 0x{SLOT:X} --verify --reboot")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
