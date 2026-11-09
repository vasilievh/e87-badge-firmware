/* Badge Display v1/v2. Portable, bounded decoder; no SDK/LVGL/flash dependencies.
 * v1: printable ASCII, limits 48/96/64 bytes. v2: strict UTF-8 (BMP only), limits
 * 64/192/96 bytes, every character accepted by the caller's glyph check. */
#ifndef BADGE_DISPLAY_PROTOCOL_H
#define BADGE_DISPLAY_PROTOCOL_H
#include <stdint.h>
#include <string.h>

#define BD_CMD 0x90
#define BD_REPLY 0x91
#define BD_VERSION 1
#define BD_VERSION_MAX 2
#define BD_TITLE_MAX 48
#define BD_BODY_MAX 96
#define BD_FOOTER_MAX 64
#define BD_PACKET_MAX 227
#define BD2_TITLE_MAX 64
#define BD2_BODY_MAX 192
#define BD2_FOOTER_MAX 96
#define BD2_PACKET_MAX (19 + BD2_TITLE_MAX + BD2_BODY_MAX + BD2_FOOTER_MAX) /* 371 */
#define BD_FLAG_UNICODE 1 /* v2 HELLO: non-ASCII text accepted (font pack present) */
enum { BD_HELLO = 1, BD_CARD = 2, BD_CLEAR = 3, BD_STATUS = 4 };
enum { BD_OK, BD_BAD_PACKET, BD_BAD_VERSION, BD_BAD_COMMAND,
       BD_BAD_VALUE, BD_INACTIVE, BD_BUSY };
typedef struct {
    uint32_t background, foreground, accent;
    uint8_t progress; /* 0..100 or 255 = hidden */
    char title[BD2_TITLE_MAX + 1];
    char body[BD2_BODY_MAX + 1];
    char footer[BD2_FOOTER_MAX + 1];
} bd_card;
typedef struct { uint8_t version, op; uint16_t seq; bd_card card; } bd_request;
typedef int (*bd_glyph_fn)(uint32_t codepoint);

static uint32_t bd_rgb(const uint8_t *p)
{ return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

static int bd_ascii_glyph(uint32_t c) { return c >= 32 && c <= 126; }

/* Reject unsupported bytes/characters instead of silently drawing empty glyphs.
 * UTF-8 (v2 only): 1..3-byte sequences, no overlong forms, surrogates, or 4-byte
 * (beyond U+FFFF) sequences. Newline is allowed only where multiline is set. */
static int bd_text_valid(const uint8_t *p, unsigned n, int multiline, int utf8, bd_glyph_fn glyph)
{
    unsigned i = 0, len;
    uint32_t c;
    while (i < n) {
        c = p[i];
        len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 0;
        if (!len || len > n - i || (len > 1 && !utf8)) return 0;
        if (len == 2) {
            if ((p[i+1] & 0xC0) != 0x80) return 0;
            c = (c & 0x1F) << 6 | (p[i+1] & 0x3F);
            if (c < 0x80) return 0;
        } else if (len == 3) {
            if ((p[i+1] & 0xC0) != 0x80 || (p[i+2] & 0xC0) != 0x80) return 0;
            c = (c & 0x0F) << 12 | (uint32_t)(p[i+1] & 0x3F) << 6 | (p[i+2] & 0x3F);
            if (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF)) return 0;
        }
        i += len;
        if (c == '\n' && multiline) continue;
        if (c < 32 || c == 127 || !glyph(c)) return 0;
    }
    return 1;
}

/* Validates completely before writing; out == NULL validates only. On failure
 * *out is untouched, so the caller may decode straight into its mailbox. */
static int bd_decode_ex(const uint8_t *p, unsigned n, bd_request *out, bd_glyph_fn glyph)
{
    unsigned a, b, c, v2;
    if (!p || n < 6 || p[0] != 'B' || p[1] != 'D') return BD_BAD_PACKET;
    if (p[2] < 1 || p[2] > BD_VERSION_MAX) return BD_BAD_VERSION;
    v2 = p[2] == 2;
    if (n > (v2 ? BD2_PACKET_MAX : BD_PACKET_MAX)) return BD_BAD_PACKET;
    if (p[3] == BD_HELLO || p[3] == BD_CLEAR || p[3] == BD_STATUS) {
        if (n != 6) return BD_BAD_PACKET;
        a = b = c = 0;
    } else if (p[3] == BD_CARD) {
        if (n < 19) return BD_BAD_PACKET;
        a = p[16]; b = p[17]; c = p[18];
        if (a > (v2 ? BD2_TITLE_MAX : BD_TITLE_MAX) || b > (v2 ? BD2_BODY_MAX : BD_BODY_MAX) ||
            c > (v2 ? BD2_FOOTER_MAX : BD_FOOTER_MAX) || n != 19 + a + b + c)
            return BD_BAD_PACKET;
        if (!v2) glyph = bd_ascii_glyph;
        if ((p[15] > 100 && p[15] != 255) || !bd_text_valid(p+19, a, 0, v2, glyph) ||
            !bd_text_valid(p+19+a, b, 1, v2, glyph) || !bd_text_valid(p+19+a+b, c, 0, v2, glyph))
            return BD_BAD_VALUE;
    } else return BD_BAD_COMMAND;
    if (!out) return BD_OK;
    memset(out, 0, sizeof(*out));
    out->version = p[2];
    out->op = p[3];
    out->seq = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    if (out->op == BD_CARD) {
        out->card.background = bd_rgb(p+6);
        out->card.foreground = bd_rgb(p+9);
        out->card.accent = bd_rgb(p+12);
        out->card.progress = p[15];
        memcpy(out->card.title, p+19, a);
        memcpy(out->card.body, p+19+a, b);
        memcpy(out->card.footer, p+19+a+b, c);
    }
    return BD_OK;
}

/* ASCII-only decoding (v2 text is limited to printable ASCII without a font pack). */
static int bd_decode(const uint8_t *p, unsigned n, bd_request *out)
{
    if (!out) return BD_BAD_PACKET;
    return bd_decode_ex(p, n, out, bd_ascii_glyph);
}
#endif
