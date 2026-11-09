/* Native harness executes the actual firmware decoder + mailbox/UI adapter.
 * LVGL/radio stubs count calls; no hardware is accessed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef struct { int unused; } lv_obj_t;
typedef struct { int unused; } lv_event_t;
typedef struct { int unused; } lv_font_t;
static lv_font_t lv_font_montserrat_36, lv_font_montserrat_14;
static lv_obj_t objects[10];
static int allocations, ui_calls, radio, depth;
static lv_obj_t menu;
static lv_obj_t *g_menu_scr = &menu;
static u8 g_badge_mouse_mode;
static u8 response[32];
static int response_len;
#define LV_PART_INDICATOR 1
#define LV_OBJ_FLAG_HIDDEN 2
#define LV_OBJ_FLAG_SCROLLABLE 3
#define LV_ALIGN_TOP_MID 4
#define LV_LABEL_LONG_DOT 5
#define LV_TEXT_ALIGN_CENTER 6
#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_EVENT_SHORT_CLICKED 7
#define LV_ANIM_OFF 0
#define OS_SR_ALLOC()
#define OS_ENTER_CRITICAL() (++depth)
#define OS_EXIT_CRITICAL() (--depth)
static lv_obj_t *new_obj(lv_obj_t *parent) { (void)parent; ++ui_calls; assert(allocations < 10); return &objects[allocations++]; }
#define lv_obj_create new_obj
#define lv_label_create new_obj
#define lv_btn_create new_obj
#define lv_color_hex(c) (c)
#define STUB(name, params) static void name params { ++ui_calls; }
STUB(lv_obj_set_style_bg_color, (lv_obj_t *o, uint32_t c, int s))
STUB(lv_obj_set_style_text_color, (lv_obj_t *o, uint32_t c, int s))
STUB(lv_obj_set_style_text_font, (lv_obj_t *o, const lv_font_t *f, int s))
STUB(lv_obj_set_style_text_align, (lv_obj_t *o, int a, int s))
STUB(lv_obj_set_style_opa, (lv_obj_t *o, int a, int s))
STUB(lv_obj_set_style_bg_opa, (lv_obj_t *o, int a, int s))
STUB(lv_obj_remove_style_all, (lv_obj_t *o))
STUB(lv_obj_set_width, (lv_obj_t *o, int w))
STUB(lv_obj_add_flag, (lv_obj_t *o, int f))
STUB(lv_obj_clear_flag, (lv_obj_t *o, int f))
STUB(lv_label_set_text, (lv_obj_t *o, const char *t))
STUB(lv_obj_set_size, (lv_obj_t *o, int w, int h))
STUB(lv_obj_align, (lv_obj_t *o, int a, int x, int y))
STUB(lv_label_set_long_mode, (lv_obj_t *o, int m))
STUB(lv_obj_add_event_cb, (lv_obj_t *o, void (*cb)(lv_event_t *), int e, void *d))
STUB(lv_scr_load, (lv_obj_t *o))
STUB(kp_set_one, (lv_obj_t *o))
STUB(kp_set_menu, (void))
static int badge_ble_battery_gate(void) { return 0; }
static void ble_on(void) { radio = 1; }
static void ble_off(void) { radio = 0; }
void rcsp_hid_att_require_encryption(u8 on) { (void)on; }
void badge_display_reply(const u8 *p, u16 n) { assert(n <= sizeof(response)); memcpy(response,p,n); response_len=n; }
#include "badge_display_ui.h"

/* Test glyph set: ASCII plus Cyrillic U+0410..U+044F (as if a font pack were loaded). */
static int cyrillic(uint32_t c) { return (c >= 32 && c <= 126) || (c >= 0x410 && c <= 0x44F); }

static int text_ok(const bd_request *r)
{
    return strlen(r->card.title) <= BD2_TITLE_MAX && strlen(r->card.body) <= BD2_BODY_MAX &&
           strlen(r->card.footer) <= BD2_FOOTER_MAX;
}

static int card2(const char *title, const char *body, int version, bd_glyph_fn glyph, bd_request *out)
{
    uint8_t packet[400] = {'B','D',0,2,1,0, 0,0,0, 0,0,0, 0,0,0, 255};
    unsigned a = (unsigned)strlen(title), b = (unsigned)strlen(body);
    packet[2] = (uint8_t)version; packet[16] = (uint8_t)a; packet[17] = (uint8_t)b; packet[18] = 0;
    memcpy(packet + 19, title, a); memcpy(packet + 19 + a, body, b);
    return bd_decode_ex(packet, 19 + a + b, out, glyph);
}

static void v2_tests(void)
{
    bd_request out;
    char long_title[66];
    /* Привет / Größe */
    assert(card2("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "", 2, cyrillic, &out) == BD_OK);
    assert(!strcmp(out.card.title, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") && out.version == 2);
    assert(card2("\xD0\x9F", "", 1, cyrillic, &out) == BD_BAD_VALUE);         /* v1 stays ASCII */
    assert(card2("\xD0\x9F", "", 2, bd_ascii_glyph, &out) == BD_BAD_VALUE);   /* no pack */
    assert(card2("Gr\xC3\xB6\xC3\x9F" "e", "", 2, cyrillic, &out) == BD_BAD_VALUE); /* not in set */
    assert(card2("\xC0\x80", "", 2, cyrillic, &out) == BD_BAD_VALUE);         /* overlong NUL */
    assert(card2("\xC1\xBF", "", 2, cyrillic, &out) == BD_BAD_VALUE);         /* overlong '?' */
    assert(card2("\xE0\x80\xAF", "", 2, cyrillic, &out) == BD_BAD_VALUE);    /* overlong 3-byte */
    assert(card2("\xED\xA0\x80", "", 2, cyrillic, &out) == BD_BAD_VALUE);    /* surrogate */
    assert(card2("\xF0\x9F\x98\x80", "", 2, cyrillic, &out) == BD_BAD_VALUE); /* beyond U+FFFF */
    assert(card2("\xD0", "", 2, cyrillic, &out) == BD_BAD_VALUE);              /* truncated */
    assert(card2("\x80", "", 2, cyrillic, &out) == BD_BAD_VALUE);              /* stray continuation */
    assert(card2("\xD0\x41", "", 2, cyrillic, &out) == BD_BAD_VALUE);         /* bad continuation */
    assert(card2("a\nb", "", 2, cyrillic, &out) == BD_BAD_VALUE);              /* newline: body only */
    assert(card2("", "a\nb", 2, cyrillic, &out) == BD_OK);
    assert(card2("\x7F", "", 2, cyrillic, &out) == BD_BAD_VALUE);
    memset(long_title, 'A', 65); long_title[65] = 0;
    assert(card2(long_title, "", 2, cyrillic, &out) == BD_BAD_PACKET);
    long_title[64] = 0; assert(card2(long_title, "", 2, cyrillic, &out) == BD_OK);
    long_title[49] = 0; assert(card2(long_title, "", 1, cyrillic, &out) == BD_BAD_PACKET);
    { /* Structured UTF-8 fuzz: any invalid or unsupported fragment rejects the card. */
        static const char *good[] = {"A", " ", "~", "\xD0\x9F", "\xD1\x8F", "\xD0\x90"};
        static const char *bad[] = {"\xC0\x80", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF0\x9F\x98\x80",
                                    "\x80", "\x7F", "\x01", "\xD0", "\xC3\xB6", "\n"};
        unsigned trial, k;
        srand(2);
        for (trial = 0; trial < 20000; ++trial) {
            char text[80] = "";
            int expect_ok = 1;
            unsigned parts = 1 + (unsigned)rand() % 8;
            for (k = 0; k < parts; ++k) {
                int use_bad = rand() % 6 == 0;
                const char *frag = use_bad ? bad[rand() % 10] : good[rand() % 6];
                if (use_bad) expect_ok = 0;
                strcat(text, frag);
            }
            assert((card2(text, "", 2, cyrillic, &out) == BD_OK) == expect_ok);
        }
    }
    { /* largest v2 card: 371 bytes; one more byte is rejected */
        static uint8_t big[372] = {'B','D',2,2,1,0, 0,0,0, 0,0,0, 0,0,0, 255, 64,192,96};
        memset(big + 19, 'x', 352);
        assert(bd_decode_ex(big, 371, &out, cyrillic) == BD_OK && strlen(out.card.body) == 192);
        assert(bd_decode_ex(big, 372, &out, cyrillic) == BD_BAD_PACKET);
        assert(bd_decode_ex(big, 371, NULL, cyrillic) == BD_OK);
    }
}

int main(void)
{
    unsigned i, n;
    bd_request out, before;
    uint8_t hello[] = {'B','D',1,1,7,0};
    uint8_t card[227] = {'B','D',1,2,8,0, 1,2,3, 4,5,6, 7,8,9, 42,48,96,64};
    memset(card+19, 'A', 208);
    assert(bd_decode(NULL, 6, &out) == BD_BAD_PACKET);
    assert(bd_decode(hello, sizeof(hello), NULL) == BD_BAD_PACKET);
    assert(bd_decode(card, sizeof(card), &out) == BD_OK);
    assert(out.seq == 8 && out.card.background == 0x010203);
    assert(strlen(out.card.title) == 48 && strlen(out.card.body) == 96 && strlen(out.card.footer) == 64);
    memset(&before, 0xa5, sizeof(before));
    for (n=0; n<sizeof(card); ++n) {
        out = before;
        assert(bd_decode(card,n,&out) != BD_OK);
        assert(memcmp(&out,&before,sizeof(out)) == 0);
    }
    card[16] = 49; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_PACKET); card[16] = 48;
    card[15] = 101; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_VALUE); card[15] = 42;
    card[19] = 0xff; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_VALUE); card[19]='A';
    card[2] = 3; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_VERSION); card[2]=1;
    card[2] = 0; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_VERSION); card[2]=1;
    card[2] = 2; assert(bd_decode(card,sizeof(card),&out) == BD_OK && out.version == 2); card[2]=1;
    v2_tests();
    /* Deterministic malformed-input fuzz: no state changes on rejection. */
    srand(87);
    for (i=0; i<20000; ++i) {
        uint8_t fuzz[260];
        n = (unsigned)rand() % sizeof(fuzz);
        for (unsigned j=0; j<n; ++j) fuzz[j]=(uint8_t)rand();
        if (n >= 6 && i % 2 == 0) {
            fuzz[0]='B'; fuzz[1]='D'; fuzz[2]=(uint8_t)(1 + i % 4 / 2); fuzz[3]=(uint8_t)(1 + i % 5);
        }
        out=before;
        if (bd_decode_ex(fuzz,n,&out,cyrillic) != BD_OK) assert(memcmp(&out,&before,sizeof(out)) == 0);
        else assert(text_ok(&out));
    }
    badge_display_command(0x90,hello,sizeof(hello));
    assert(response[6] == BD_INACTIVE && allocations == 0);
    display_open_cb(NULL); assert(radio && g_badge_display_mode);
    ui_calls=0;
    badge_display_command(0x90,hello,sizeof(hello));
    assert(ui_calls == 0 && bd_pending); /* BLE thread must never call LVGL */
    badge_display_command(0x90,hello,sizeof(hello)); assert(response[6] == BD_BUSY);
    badge_display_tick(); assert(response_len == 14 && response[6] == BD_OK && depth == 0);
    { /* v2 HELLO without a font pack: 13-byte reply, ASCII only (flags 0, font id 0). */
        uint8_t hello2[] = {'B','D',2,1,9,0};
        badge_display_command(0x90,hello2,sizeof(hello2)); badge_display_tick();
        assert(response_len == 13 && response[2] == 2 && response[3] == 0x81 && response[4] == 9);
        assert(response[6] == BD_OK && response[7] == 64 && response[8] == 192 && response[9] == 96);
        assert(response[10] == 0 && response[11] == 0 && response[12] == 0);
        hello2[2] = 3; badge_display_command(0x90,hello2,sizeof(hello2));
        assert(response_len == 9 && response[2] == 1 && response[6] == BD_BAD_VERSION && !bd_pending);
    }
    { /* Non-ASCII v2 text is refused without a font pack, before touching the mailbox. */
        static const uint8_t ru[] = {'B','D',2,2,10,0, 0,0,0, 255,255,255, 0,0,0, 255, 4,0,0,
                                     0xD0,0x9F,0xD1,0x80};
        badge_display_command(0x90,ru,sizeof(ru));
        assert(response[2] == 2 && response[6] == BD_BAD_VALUE && !bd_pending);
    }
    badge_display_command(0x90,card,sizeof(card)); assert(ui_calls == 0);
    badge_display_tick(); assert(ui_calls > 0 && response[6] == BD_OK && bd_revision == 1);
    assert(!bd_pending && strlen(bd_mailbox.card.title) == 48);
    { uint8_t clear[] = {'B','D',2,3,11,0};
      badge_display_command(0x90,clear,sizeof(clear)); badge_display_tick();
      assert(response[6] == BD_OK && bd_revision == 2 && !bd_mailbox.card.title[0] &&
             bd_mailbox.card.progress == 255 && response[7] == 2); }
    display_back_cb(NULL); assert(!radio && !g_badge_display_mode && !bd_pending);
    display_open_cb(NULL); assert(allocations == 7); /* screen reused, no new objects */
    puts("display native: bounds, fuzz, mailbox, acknowledgements, lifecycle OK");
    return 0;
}
