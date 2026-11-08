/* Badge Display v1. Portable, bounded decoder; no SDK/LVGL/flash dependencies. */
#ifndef BADGE_DISPLAY_PROTOCOL_H
#define BADGE_DISPLAY_PROTOCOL_H
#include <stdint.h>
#include <string.h>

#define BD_CMD 0x90
#define BD_REPLY 0x91
#define BD_VERSION 1
#define BD_TITLE_MAX 48
#define BD_BODY_MAX 96
#define BD_FOOTER_MAX 64
#define BD_PACKET_MAX 227
enum { BD_HELLO = 1, BD_CARD = 2, BD_CLEAR = 3, BD_STATUS = 4 };
enum { BD_OK, BD_BAD_PACKET, BD_BAD_VERSION, BD_BAD_COMMAND,
       BD_BAD_VALUE, BD_INACTIVE, BD_BUSY };
typedef struct {
    uint32_t background, foreground, accent;
    uint8_t progress; /* 0..100 or 255 = hidden */
    char title[BD_TITLE_MAX + 1];
    char body[BD_BODY_MAX + 1];
    char footer[BD_FOOTER_MAX + 1];
} bd_card;
typedef struct { uint8_t op; uint16_t seq; bd_card card; } bd_request;

static uint32_t bd_rgb(const uint8_t *p)
{ return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

/* v1 exposes the embedded font's printable ASCII subset. Reject unsupported
 * bytes instead of silently displaying empty glyphs. Newline allowed in body. */
static int bd_text_valid(const uint8_t *p, unsigned n, int multiline)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        if ((p[i] < 32 || p[i] > 126) && !(multiline && p[i] == '\n')) return 0;
    return 1;
}

static int bd_decode(const uint8_t *p, unsigned n, bd_request *out)
{
    unsigned a, b, c;
    bd_request next;
    if (!p || !out || n < 6 || n > BD_PACKET_MAX || p[0] != 'B' || p[1] != 'D')
        return BD_BAD_PACKET;
    if (p[2] != BD_VERSION) return BD_BAD_VERSION;
    memset(&next, 0, sizeof(next));
    next.op = p[3];
    next.seq = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    if (next.op == BD_HELLO || next.op == BD_CLEAR || next.op == BD_STATUS) {
        if (n != 6) return BD_BAD_PACKET;
    } else if (next.op == BD_CARD) {
        if (n < 19) return BD_BAD_PACKET;
        a = p[16]; b = p[17]; c = p[18];
        if (a > BD_TITLE_MAX || b > BD_BODY_MAX || c > BD_FOOTER_MAX || n != 19 + a + b + c)
            return BD_BAD_PACKET;
        if ((p[15] > 100 && p[15] != 255) || !bd_text_valid(p+19, a, 0) ||
            !bd_text_valid(p+19+a, b, 1) || !bd_text_valid(p+19+a+b, c, 0))
            return BD_BAD_VALUE;
        next.card.background = bd_rgb(p+6);
        next.card.foreground = bd_rgb(p+9);
        next.card.accent = bd_rgb(p+12);
        next.card.progress = p[15];
        memcpy(next.card.title, p+19, a);
        memcpy(next.card.body, p+19+a, b);
        memcpy(next.card.footer, p+19+a+b, c);
    } else return BD_BAD_COMMAND;
    *out = next; /* No partial state change on failure. */
    return BD_OK;
}
#endif
