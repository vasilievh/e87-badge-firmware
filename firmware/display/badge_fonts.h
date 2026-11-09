/* Device-local BFN1 font pack, read one glyph at a time from flash.
 * Format: tools/make_badge_fonts.py. No filesystem, malloc or XIP pointer: only
 * the CODE region is memory-mapped, so reads go through tzflash_read. UI task only.
 * Without a valid pack, callers keep the built-in 14px font (menu/Update stay usable). */
#ifndef BADGE_FONTS_H
#define BADGE_FONTS_H
#include <stdint.h>
#include <string.h>
#define BADGE_FONT_SLOT 0x310000u
#define BADGE_FONT_SLOT_SIZE 0x2E000u /* ends at VIRFAT 0x33E000 */
#define BADGE_FONT_MAX 4
#define BADGE_FONT_RANGES 32
#define BADGE_FONT_GLYPHS 512
#define BADGE_FONT_BITMAP_MAX 1024
#define BADGE_TITLE_FONT 1 /* replaces the Display title's built-in 36px font */
extern int tzflash_read(void *buf, unsigned int len, unsigned int offset);

typedef struct { uint8_t px, line_height, base_line, bpp; uint32_t table, bitmaps; } badge_font_rec;
typedef struct { uint8_t off[3], adv, w, h; int8_t x, y; } badge_glyph_rec;
typedef char badge_glyph_rec_is_8_bytes[sizeof(badge_glyph_rec) == 8 ? 1 : -1];
static struct {
    uint32_t size;
    uint8_t fonts, ranges;
    uint16_t range[BADGE_FONT_RANGES][2];
    badge_font_rec font[BADGE_FONT_MAX];
} badge_fp;
static lv_font_t badge_fp_lv[BADGE_FONT_MAX];
static uint8_t badge_fp_bitmap[BADGE_FONT_BITMAP_MAX];
static struct { const lv_font_t *font; uint32_t letter; uint8_t loaded; badge_glyph_rec g; } badge_fp_last;

#define BADGE_LE16(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8)
#define BADGE_LE32(p) (BADGE_LE16(p) | BADGE_LE16((p)+2) << 16)

static uint16_t badge_fp_crc(const uint8_t *p, unsigned n, uint16_t crc)
{
    unsigned i;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (i = 0; i < 8; ++i) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static uint32_t badge_fp_len(const badge_glyph_rec *g) { return ((uint32_t)g->w * g->h * 4u + 7u) / 8u; }
static uint32_t badge_fp_off(const badge_glyph_rec *g)
{ return (uint32_t)g->off[0] | (uint32_t)g->off[1] << 8 | (uint32_t)g->off[2] << 16; }

static int badge_fp_index(uint32_t letter)
{
    unsigned i, base = 0;
    for (i = 0; i < badge_fp.ranges; ++i) {
        uint32_t start = badge_fp.range[i][0], count = badge_fp.range[i][1];
        if (letter >= start && letter - start < count) return (int)(base + letter - start);
        base += count;
    }
    return -1;
}

/* Every offset comes from flash, so it is bounded here even though the pack has a CRC. */
static const badge_glyph_rec *badge_fp_glyph(const lv_font_t *font, uint32_t letter)
{
    const badge_font_rec *r = (const badge_font_rec *)font->dsc;
    badge_glyph_rec g;
    uint32_t at, len;
    int i;
    if (badge_fp_last.font == font && badge_fp_last.letter == letter) return &badge_fp_last.g;
    i = badge_fp_index(letter);
    if (i < 0 || tzflash_read(&g, sizeof(g), BADGE_FONT_SLOT + r->table + 8u * (unsigned)i) < 0) return NULL;
    at = r->bitmaps + badge_fp_off(&g); len = badge_fp_len(&g);
    if (len > BADGE_FONT_BITMAP_MAX || at < r->bitmaps || at > badge_fp.size || len > badge_fp.size - at)
        return NULL;
    badge_fp_last.font = font; badge_fp_last.letter = letter; badge_fp_last.loaded = 0;
    badge_fp_last.g = g;
    return &badge_fp_last.g;
}

static bool badge_fp_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d, uint32_t letter, uint32_t next)
{
    const badge_glyph_rec *g = badge_fp_glyph(font, letter);
    (void)next;
    if (!g) return false;
    d->adv_w = g->adv; d->box_w = g->w; d->box_h = g->h;
    d->ofs_x = g->x; d->ofs_y = g->y; d->bpp = 4; d->is_placeholder = 0;
    return true;
}

static const uint8_t *badge_fp_bitmap_cb(const lv_font_t *font, uint32_t letter)
{
    const badge_glyph_rec *g = badge_fp_glyph(font, letter);
    const badge_font_rec *r = (const badge_font_rec *)font->dsc;
    if (!g) return NULL;
    if (!badge_fp_last.loaded) {
        if (badge_fp_len(g) && tzflash_read(badge_fp_bitmap, badge_fp_len(g), BADGE_FONT_SLOT + r->bitmaps + badge_fp_off(g)) < 0)
            return NULL;
        badge_fp_last.loaded = 1;
    }
    return badge_fp_bitmap;
}

/* Validate the whole record before publishing any font. Returns the font count. */
static unsigned badge_fonts_init(void)
{
    uint8_t h[16], chunk[64];
    uint32_t size, at, n, glyphs, sum = 0, prev_end = 0, meta;
    uint16_t crc = 0xffff;
    unsigned i, fonts, ranges;
    memset(&badge_fp, 0, sizeof(badge_fp));
    memset(&badge_fp_last, 0, sizeof(badge_fp_last));
    if (tzflash_read(h, sizeof(h), BADGE_FONT_SLOT) < 0 || memcmp(h, "BFN1", 4) || h[4] != 1 || h[7]) return 0;
    fonts = h[5]; ranges = h[6]; size = BADGE_LE32(h+8); glyphs = BADGE_LE16(h+14);
    meta = 16u + 4u * ranges + 12u * fonts;
    if (!fonts || fonts > BADGE_FONT_MAX || !ranges || ranges > BADGE_FONT_RANGES ||
        !glyphs || glyphs > BADGE_FONT_GLYPHS || size < meta || size > BADGE_FONT_SLOT_SIZE) return 0;
    for (at = 16; at < size; at += n) {
        n = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
        if (tzflash_read(chunk, n, BADGE_FONT_SLOT + at) < 0) return 0;
        crc = badge_fp_crc(chunk, n, crc);
    }
    if (crc != BADGE_LE16(h+12)) return 0;
    for (i = 0; i < ranges; ++i) {
        if (tzflash_read(chunk, 4, BADGE_FONT_SLOT + 16u + 4u * i) < 0) return 0;
        badge_fp.range[i][0] = (uint16_t)BADGE_LE16(chunk);
        badge_fp.range[i][1] = (uint16_t)BADGE_LE16(chunk+2);
        if (!badge_fp.range[i][1] || badge_fp.range[i][0] < prev_end ||
            (uint32_t)badge_fp.range[i][0] + badge_fp.range[i][1] > 0x10000u) return 0;
        prev_end = (uint32_t)badge_fp.range[i][0] + badge_fp.range[i][1];
        sum += badge_fp.range[i][1];
    }
    /* Display v1 sends printable ASCII; a pack that cannot render all of it is rejected. */
    if (sum != glyphs || badge_fp.range[0][0] != 32 || badge_fp.range[0][1] != 95) return 0;
    for (i = 0; i < fonts; ++i) {
        badge_font_rec *r = &badge_fp.font[i];
        if (tzflash_read(chunk, 12, BADGE_FONT_SLOT + 16u + 4u * ranges + 12u * i) < 0) return 0;
        r->px = chunk[0]; r->line_height = chunk[1]; r->base_line = chunk[2]; r->bpp = chunk[3];
        r->table = BADGE_LE32(chunk+4); r->bitmaps = BADGE_LE32(chunk+8);
        if (r->px < 8 || r->px > 48 || (i && r->px <= badge_fp.font[i-1].px) || r->bpp != 4 ||
            r->base_line >= r->line_height || r->table < meta || r->table > size ||
            glyphs * 8u > size - r->table || r->bitmaps > size) return 0;
    }
    badge_fp.size = size;
    for (i = 0; i < fonts; ++i) {
        lv_font_t *f = &badge_fp_lv[i];
        memset(f, 0, sizeof(*f));
        f->get_glyph_dsc = badge_fp_dsc; f->get_glyph_bitmap = badge_fp_bitmap_cb;
        f->line_height = badge_fp.font[i].line_height; f->base_line = badge_fp.font[i].base_line;
        f->underline_position = -2; f->underline_thickness = 1; f->dsc = &badge_fp.font[i];
    }
    badge_fp.ranges = (uint8_t)ranges; badge_fp.fonts = (uint8_t)fonts;
    return fonts;
}

static const lv_font_t *badge_font_px(unsigned px)
{
    unsigned i;
    for (i = 0; i < badge_fp.fonts; ++i) if (badge_fp.font[i].px == px) return &badge_fp_lv[i];
    return NULL;
}

/* Largest pack size (36, 24, 20) whose single line fits the 240px title label. */
static const lv_font_t *bd_title_font(const char *title)
{
    static const uint8_t sizes[] = {36, 24, 20};
    const lv_font_t *font, *smallest = NULL;
    unsigned i;
    for (i = 0; i < sizeof(sizes); ++i) {
        if (!(font = badge_font_px(sizes[i]))) continue;
        smallest = font;
        if (lv_txt_get_width(title, (uint32_t)strlen(title), font, 0, 0) <= 240) return font;
    }
    return smallest ? smallest : &lv_font_montserrat_14;
}
#endif
