/* Execute the actual flash-font loader against a generated BFN1 pack in fake flash.
 * Checks parsing, LVGL callbacks, title-size selection, failed reads and that
 * corrupted packs (including adversarial ones with a recomputed CRC) never read
 * outside the record or overflow the glyph buffer. No SDK or hardware involved. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int16_t lv_coord_t;
typedef struct _lv_font_t lv_font_t;
typedef struct {
    const lv_font_t *resolved_font;
    uint16_t adv_w, box_w, box_h; int16_t ofs_x, ofs_y;
    uint8_t bpp: 4, is_placeholder: 1;
} lv_font_glyph_dsc_t;
struct _lv_font_t {
    bool (*get_glyph_dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const uint8_t *(*get_glyph_bitmap)(const lv_font_t *, uint32_t);
    lv_coord_t line_height, base_line; uint8_t subpx: 2;
    int8_t underline_position, underline_thickness;
    const void *dsc; const lv_font_t *fallback; void *user_data;
};
static lv_font_t lv_font_montserrat_14;
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}
static lv_coord_t lv_txt_get_width(const char *t, uint32_t n, const lv_font_t *f, lv_coord_t space, int flag)
{
    const char *end = t + n;
    lv_font_glyph_dsc_t d;
    int w = 0;
    (void)space; (void)flag;
    while (t < end) if (f->get_glyph_dsc(f, &d, utf8_next(&t), 0)) w += d.adv_w;
    return (lv_coord_t)w;
}

#define SLOT 0x310000u
#define SLOT_SIZE 0x2E000u
static uint8_t flash[SLOT_SIZE], pack[SLOT_SIZE];
static unsigned pack_size, read_limit = SLOT_SIZE, max_read;
static int fail_read;
int tzflash_read(void *buf, unsigned n, unsigned address)
{
    assert(address >= SLOT && n <= SLOT_SIZE && address - SLOT <= read_limit - n);
    if (n > max_read) max_read = n;
    if (fail_read) return -1;
    memcpy(buf, flash + (address - SLOT), n);
    return (int)n;
}
#include "badge_fonts.h"
static int resource_status;
int badge_resource_find(uint32_t id, uint32_t *address, uint32_t *length)
{
    assert(id==BR_FONTS);
    if (resource_status>0) { *address=SLOT; *length=pack_size; }
    return resource_status;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void install(const uint8_t *data, unsigned n)
{
    memset(flash, 0xff, sizeof(flash));
    resource_status=-1; assert(badge_fonts_init()==0);
    resource_status=0;
    memcpy(flash, data, n);
}
static void fix_crc(uint8_t *p)
{
    uint32_t n = le32(p + 8);
    uint16_t crc = n >= 16 && n <= SLOT_SIZE ? badge_fp_crc(p + 16, n - 16, 0xffff) : 0;
    p[12] = (uint8_t)crc; p[13] = (uint8_t)(crc >> 8);
}

static const unsigned supported[] = {0x20, 'A', 'z', '~', 0xC4, 0xD6, 0xDC, 0xDF, 0xE4, 0xF6, 0xFC,
                                     0x401, 0x410, 0x429, 0x44F, 0x451, 0xAB, 0xBB, 0x2014, 0x2116, 0x20AC};
static const unsigned unsupported[] = {0x1F, 0x7F, 0xA0, 0x400, 0x452, 0x4E2D, 0xFFFF, 0x1F600};

/* Independent reading of the raw record, compared against the loader's output. */
static void check_glyphs(void)
{
    unsigned f, i, cp;
    lv_font_glyph_dsc_t d;
    const uint8_t *ranges = pack + 16, *fonts = pack + 16 + 4 * pack[6];
    for (f = 0; f < pack[5]; ++f) {
        const lv_font_t *font = badge_font_px(fonts[12 * f]);
        uint32_t table = le32(fonts + 12 * f + 4), bitmaps = le32(fonts + 12 * f + 8);
        unsigned index = 0, r;
        assert(font && font->line_height == fonts[12 * f + 1] && font->base_line == fonts[12 * f + 2]);
        for (r = 0; r < pack[6]; ++r) {
            unsigned start = ranges[4 * r] | ranges[4 * r + 1] << 8, n = ranges[4 * r + 2] | ranges[4 * r + 3] << 8;
            for (i = 0; i < n; ++i, ++index) {
                const uint8_t *g = pack + table + 8 * index;
                const uint8_t *bitmap;
                unsigned len = (g[4] * g[5] * 4 + 7) / 8, off = bitmaps + (g[0] | g[1] << 8 | g[2] << 16);
                cp = start + i;
                assert(font->get_glyph_dsc(font, &d, cp, 0));
                assert(d.adv_w == g[3] && d.box_w == g[4] && d.box_h == g[5] && d.bpp == 4);
                assert(d.ofs_x == (int8_t)g[6] && d.ofs_y == (int8_t)g[7] && !d.is_placeholder);
                bitmap = font->get_glyph_bitmap(font, cp);
                assert(bitmap && !memcmp(bitmap, pack + off, len));
            }
        }
        for (i = 0; i < sizeof(unsupported) / sizeof(*unsupported); ++i) {
            assert(!font->get_glyph_dsc(font, &d, unsupported[i], 0));
            assert(!font->get_glyph_bitmap(font, unsupported[i]));
        }
        for (i = 0; i < sizeof(supported) / sizeof(*supported); ++i) assert(font->get_glyph_dsc(font, &d, supported[i], 0));
    }
}

/* Query everything an attacker-controlled but CRC-valid pack could make us read. */
static void sweep(void)
{
    unsigned f, cp;
    lv_font_glyph_dsc_t d;
    for (f = 0; f < badge_fp.fonts; ++f)
        for (cp = 0; cp < 0x2200; cp += (cp < 0x500 ? 1 : 7)) {
            if (badge_fp_lv[f].get_glyph_dsc(&badge_fp_lv[f], &d, cp, 0))
                assert(d.box_w * d.box_h * 4u <= BADGE_FONT_BITMAP_MAX * 8u);
            (void)badge_fp_lv[f].get_glyph_bitmap(&badge_fp_lv[f], cp);
        }
}

int main(int argc, char **argv)
{
    static uint8_t mutated[SLOT_SIZE];
    FILE *file;
    unsigned i, trial, accepted = 0;
    lv_font_glyph_dsc_t d;
    memset(flash, 0xff, sizeof(flash));
    assert(badge_fonts_init() == 0 && !badge_font_px(36));
    assert(bd_title_font("HELLO") == &lv_font_montserrat_14); /* erased slot: built-in font */
    /* Display v2 hooks without a pack: built-in text font, ASCII only, no font id. */
    assert(bd_text_font() == &lv_font_montserrat_14 && !bd_font_id());
    assert(bd_glyph('A') && bd_glyph('~') && !bd_glyph(0x41F) && !bd_glyph(0x7F) && !bd_glyph(0x1F));
    if (argc < 2) { puts("Fonts: erased-slot fallback OK (no generated pack supplied)"); return 0; }
    file = fopen(argv[1], "rb");
    assert(file);
    pack_size = (unsigned)fread(pack, 1, sizeof(pack), file);
    fclose(file);
    assert(pack_size > 16 && pack_size == le32(pack + 8));

    install(pack, pack_size);
    read_limit = pack_size;
    max_read = 0;
    resource_status=1;
    assert(badge_fonts_init() == pack[5]);
    check_glyphs();
    assert(max_read <= BADGE_FONT_BITMAP_MAX);
    assert(bd_title_font("HELLO") == badge_font_px(36));
    /* Display v2 hooks with a pack: pack text font, its character set, CRC as font id. */
    assert(bd_text_font() == badge_font_px(14) && bd_font_id() == (pack[12] | pack[13] << 8));
    assert(bd_glyph(0x41F) && bd_glyph(0xDF) && bd_glyph(0x2116) && bd_glyph('A'));
    assert(!bd_glyph(0x4E2D) && !bd_glyph(0x7F) && !bd_glyph(0x1F) && !bd_glyph(0x1F600));
    assert(bd_title_font("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == badge_font_px(36)); /* Привет */
    assert(bd_title_font("WWWWWWWWWWWWW") != badge_font_px(36));
    assert(bd_title_font("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW") ==
           (badge_font_px(20) ? badge_font_px(20) : badge_font_px(24))); /* smallest title size */
    fail_read = 1;
    badge_fp_last.font = NULL;
    assert(!badge_font_px(36)->get_glyph_dsc(badge_font_px(36), &d, 'Q', 0));
    assert(badge_fonts_init() == 0 && !badge_font_px(14));
    fail_read = 0;

    /* A pack without a 14px size cannot draw card text: v2 stays ASCII-only. */
    memcpy(mutated, pack, pack_size);
    if (mutated[16 + 4 * pack[6]] == 14) {
        mutated[16 + 4 * pack[6]] = 15; fix_crc(mutated);
        install(mutated, pack_size); read_limit = pack_size;
        assert(badge_fonts_init() == pack[5] && !badge_font_px(14));
        assert(!bd_font_id() && !bd_glyph(0x41F) && bd_glyph('A') && bd_text_font() == &lv_font_montserrat_14);
    }

    /* Truncation and a header claiming more bytes than were written. */
    install(pack, pack_size - 1); read_limit = SLOT_SIZE;
    assert(badge_fonts_init() == 0);
    memcpy(mutated, pack, pack_size); mutated[8] = (uint8_t)(mutated[8] + 4);
    install(mutated, pack_size); assert(badge_fonts_init() == 0);

    /* Rule violations with a correct CRC are still rejected. */
    memcpy(mutated, pack, pack_size); mutated[16] = 33; fix_crc(mutated);          /* ASCII not first */
    install(mutated, pack_size); assert(badge_fonts_init() == 0);
    memcpy(mutated, pack, pack_size); mutated[16 + 4 * pack[6] + 3] = 2; fix_crc(mutated); /* bpp */
    install(mutated, pack_size); assert(badge_fonts_init() == 0);
    memcpy(mutated, pack, pack_size); mutated[16 + 4 * pack[6] + 12] = mutated[16 + 4 * pack[6]]; fix_crc(mutated);
    install(mutated, pack_size); assert(badge_fonts_init() == 0);                   /* px order */
    memcpy(mutated, pack, pack_size); mutated[14] = (uint8_t)(mutated[14] + 1); fix_crc(mutated);
    install(mutated, pack_size); assert(badge_fonts_init() == 0);                   /* glyph count */

    /* Random single-byte corruption: the CRC must reject every one. */
    srand(1);
    for (trial = 0; trial < 3000; ++trial) {
        unsigned at = (unsigned)rand() % pack_size;
        memcpy(mutated, pack, pack_size);
        mutated[at] ^= (uint8_t)(1 + rand() % 255);
        install(mutated, pack_size); read_limit = SLOT_SIZE;
        if (at >= 16 || at == 12 || at == 13) assert(badge_fonts_init() == 0);
        else if (badge_fonts_init()) { ++accepted; read_limit = le32(mutated + 8); sweep(); }
    }
    /* Adversarial metadata/table edits with a recomputed CRC: every read stays in bounds. */
    for (trial = 0; trial < 3000; ++trial) {
        unsigned edits = 1 + (unsigned)rand() % 4, meta = 16 + 4 * pack[6] + 12 * pack[5];
        memcpy(mutated, pack, pack_size);
        for (i = 0; i < edits; ++i) {
            unsigned at = rand() % 2 ? 4 + (unsigned)rand() % (meta - 4)
                                     : le32(pack + 16 + 4 * pack[6] + 4) + (unsigned)rand() % (8 * 179);
            mutated[at] = (uint8_t)rand();
        }
        fix_crc(mutated);
        install(mutated, pack_size); read_limit = SLOT_SIZE;
        if (badge_fonts_init()) {
            ++accepted;
            assert(badge_fp.size <= SLOT_SIZE);
            read_limit = badge_fp.size;
            sweep();
        }
    }
    printf("Fonts: %u sizes, glyph metrics/bitmaps, title fit, failed reads, corruption (%u bounded accepts) OK\n",
           pack[5], accepted);
    return 0;
}
