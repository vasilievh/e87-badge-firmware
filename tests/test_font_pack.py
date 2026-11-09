"""BFN1 font pack generator: layout, limits and pixel-exact round trip (no device I/O)."""
import binascii
import importlib.util
from pathlib import Path
import struct
import pytest

pytest.importorskip("PIL")
pytest.importorskip("fontTools")
from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_badge_fonts", ROOT / "tools/make_badge_fonts.py")
fonts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fonts)


def parse(blob):
    assert blob[:4] == b"BFN1" and blob[4] == 1 and blob[7] == 0
    count, nranges = blob[5], blob[6]
    total, crc, glyphs = struct.unpack("<IHH", blob[8:16])
    assert total == len(blob) and crc == binascii.crc_hqx(blob[16:], 0xFFFF)
    ranges = [struct.unpack_from("<HH", blob, 16 + 4 * i) for i in range(nranges)]
    records = [struct.unpack_from("<BBBBII", blob, 16 + 4 * nranges + 12 * i) for i in range(count)]
    assert sum(n for _, n in ranges) == glyphs
    return ranges, records


def glyph(blob, ranges, record, char):
    index, cp = 0, ord(char)
    for start, n in ranges:
        if start <= cp < start + n:
            index += cp - start
            break
        index += n
    else:
        raise KeyError(char)
    px, line_height, base_line, bpp, table, bitmaps = record
    entry = blob[table + 8 * index: table + 8 * index + 8]
    adv, w, h, x, y = struct.unpack("<BBBbb", entry[3:])
    start = bitmaps + int.from_bytes(entry[:3], "little")
    data = blob[start:start + (w * h * 4 + 7) // 8]
    pixels = [(data[i // 2] >> (4 if i % 2 == 0 else 0)) & 15 for i in range(w * h)]
    return adv, w, h, x, y, pixels


@pytest.fixture(scope="module")
def default_pack():
    return fonts.build()


def test_default_pack_layout_and_limits(default_pack):
    ranges, records = parse(default_pack)
    assert len(default_pack) <= fonts.SLOT_SIZE
    assert ranges[0] == (32, 95) and len(ranges) <= fonts.MAX_RANGES
    assert sum(n for _, n in ranges) == len(fonts.CHARSET) == 179
    assert [r[0] for r in records] == [14, 20, 24, 36] and all(r[3] == 4 for r in records)
    for px, line_height, base_line, _, table, bitmaps in records:
        assert 0 <= base_line < line_height and bitmaps == table + 8 * 179
    assert fonts.build() == default_pack  # deterministic


@pytest.mark.parametrize("char", ["A", "g", "Щ", "ё", "Ä", "ß", "№", "„"])
def test_pixels_round_trip_exactly(default_pack, char):
    ranges, records = parse(default_pack)
    record = records[-1]
    adv, w, h, x, y, pixels = glyph(default_pack, ranges, record, char)
    font = ImageFont.truetype(str(fonts.DEFAULT_FONT), record[0])
    mask, (dx, dy) = font.getmask2(char, mode="L", anchor="ls")
    x0, y0, x1, y1 = mask.getbbox()
    assert (w, h, x, y) == (x1 - x0, y1 - y0, dx + x0, -(dy + y1))
    assert adv == round(font.getlength(char))
    expected = [round(mask.getpixel((i, j)) * 15 / 255) for j in range(y0, y1) for i in range(x0, x1)]
    assert pixels == expected
    # Glyph boxes fit the advertised line metrics, as LVGL positions them.
    assert y >= -record[2] and y + h <= record[1] - record[2]


def test_space_has_advance_but_no_bitmap(default_pack):
    ranges, records = parse(default_pack)
    adv, w, h, *_ = glyph(default_pack, ranges, records[0], " ")
    assert adv > 0 and w == h == 0


def test_largest_supported_pack_fits_flash_and_glyph_buffer():
    blob = fonts.build(sizes=(14, 24, 36, 48))
    ranges, records = parse(blob)
    assert len(blob) <= fonts.SLOT_SIZE
    for record in records:
        for char in fonts.CHARSET:
            _, w, h, *_ = glyph(blob, ranges, record, char)
            assert (w * h * 4 + 7) // 8 <= fonts.BITMAP_MAX


@pytest.mark.parametrize("sizes", [(), (14, 14), (7,), (49,), (14, 20, 24, 36, 48)])
def test_rejects_unsupported_sizes(sizes):
    with pytest.raises(ValueError):
        fonts.build(sizes=sizes)


def test_rejects_charset_without_ascii_or_missing_glyphs():
    with pytest.raises(ValueError, match="ASCII"):
        fonts.build(charset=fonts.CHARSET[1:])
    with pytest.raises(ValueError, match="U\\+4E2D"):
        fonts.build(charset=fonts.CHARSET + ["中"])


def test_cli_writes_outside_repository(tmp_path, capsys):
    out = tmp_path / "fonts.bin"
    assert fonts.main([str(out), "--sizes", "14,36"]) == 0
    _, records = parse(out.read_bytes())
    assert [r[0] for r in records] == [14, 36]
    output = capsys.readouterr().out
    assert f"python -m qix_ble.resources ADDRESS upload fonts {out} --reboot" in output
    assert "qix rawflash" not in output
