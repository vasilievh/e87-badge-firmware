/* Included in lvgl_ui.c after ble_on/off, kp_set_one/menu and the battery gate.
 * Only the UI task calls LVGL. BLE RX decodes one bounded request straight into the
 * mailbox while it is free; the UI task renders from it and only then frees it.
 * No request or card (up to ~370 bytes) is copied onto either task's stack. */
#include "badge_display_protocol.h"
volatile u8 g_badge_display_mode;
static volatile u8 bd_pending;
static bd_request bd_mailbox;
static uint16_t bd_revision;
static lv_obj_t *bd_screen, *bd_title, *bd_body, *bd_footer, *bd_bar, *bd_fill, *bd_back;
extern void badge_display_reply(const u8 *data, u16 len);

#ifndef BADGE_TITLE_FONT
/* Without the system overlay's flash font pack (badge_fonts.h): ASCII only.
 * Upstream strips the 36px font to ASCII 32..90. Fall back to the complete
 * 14px ASCII font for lowercase/punctuation rather than losing glyphs. */
static const lv_font_t *bd_title_font(const char *title)
{
    for (; *title; ++title) if ((unsigned char)*title > 90) return &lv_font_montserrat_14;
    return &lv_font_montserrat_36;
}
static const lv_font_t *bd_text_font(void) { return &lv_font_montserrat_14; }
static int bd_glyph(uint32_t c) { return bd_ascii_glyph(c); }
static uint16_t bd_font_id(void) { return 0; }
#endif

static void bd_ack(uint8_t version, uint8_t op, uint16_t seq, uint8_t status, int hello)
{
    uint8_t p[14] = {'B', 'D', version, (uint8_t)(op | 0x80),
                    (uint8_t)seq, (uint8_t)(seq >> 8), status};
    uint16_t font = bd_font_id();
    unsigned n = 9;
    p[7] = (uint8_t)bd_revision; p[8] = (uint8_t)(bd_revision >> 8);
    if (hello && status == BD_OK && version == 1) {
        p[7] = 0x68; p[8] = 1; p[9] = 0x68; p[10] = 1; /* 360 x 360 LE */
        p[11] = BD_TITLE_MAX; p[12] = BD_BODY_MAX; p[13] = BD_FOOTER_MAX;
        n = 14;
    } else if (hello && status == BD_OK) {
        /* v2: limits, flags, font pack id (CRC16; 0 = ASCII only). */
        p[7] = BD2_TITLE_MAX; p[8] = BD2_BODY_MAX; p[9] = BD2_FOOTER_MAX;
        p[10] = font ? BD_FLAG_UNICODE : 0; p[11] = (uint8_t)font; p[12] = (uint8_t)(font >> 8);
        n = 13;
    }
    /* Every reply frame is at most 20 bytes, even at the default ATT MTU 23. */
    badge_display_reply(p, (u16)n);
}

/* Called by the BLE task, never by LVGL. Validate, claim the free mailbox, decode
 * into it, then publish. The UI task reads the mailbox only while bd_pending is set. */
int badge_display_command(u8 cmd, const u8 *p, u16 n)
{
    int status, claimed = 0;
    uint8_t op, version;
    uint16_t seq;
    if (cmd != BD_CMD) return 0;
    /* Do not answer unrelated vendor 0x90 payloads. */
    if (n < 6 || p[0] != 'B' || p[1] != 'D') return 1;
    version = p[2] >= 1 && p[2] <= BD_VERSION_MAX ? p[2] : BD_VERSION;
    op = p[3]; seq = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    status = bd_decode_ex(p, n, NULL, bd_glyph);
    if (status == BD_OK) {
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        if (!g_badge_display_mode) status = BD_INACTIVE;
        else if (bd_pending) status = BD_BUSY;
        else claimed = 1;
        OS_EXIT_CRITICAL();
    }
    if (claimed) {
        bd_decode_ex(p, n, &bd_mailbox, bd_glyph); /* already validated */
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        if (g_badge_display_mode) bd_pending = 1; else status = BD_INACTIVE;
        OS_EXIT_CRITICAL();
    }
    if (status != BD_OK) bd_ack(version, op, seq, (uint8_t)status, 0);
    return 1;
}

static void bd_render(const bd_card *c)
{
    lv_obj_t *labels[3] = {bd_title, bd_body, bd_footer};
    int i;
    lv_obj_set_style_text_font(bd_title, bd_title_font(c->title), 0);
    lv_obj_set_style_bg_color(bd_screen, lv_color_hex(c->background), 0);
    for (i = 0; i < 3; ++i)
        lv_obj_set_style_text_color(labels[i], lv_color_hex(c->foreground), 0);
    lv_label_set_text(bd_title, c->title);
    lv_label_set_text(bd_body, c->body);
    lv_label_set_text(bd_footer, c->footer);
    lv_obj_set_style_bg_color(bd_fill, lv_color_hex(c->accent), 0);
    if (c->progress == 255) lv_obj_add_flag(bd_bar, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_clear_flag(bd_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(bd_fill, 220 * c->progress / 100);
        if (c->progress == 0) lv_obj_add_flag(bd_fill, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(bd_fill, LV_OBJ_FLAG_HIDDEN);
    }
}

static void badge_display_tick(void)
{
    uint8_t version, op;
    uint16_t seq;
    int ready;
    {
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        ready = g_badge_display_mode && bd_pending;
        OS_EXIT_CRITICAL();
    }
    if (!ready) return;
    /* The BLE task never writes the mailbox while bd_pending is set. */
    version = bd_mailbox.version; op = bd_mailbox.op; seq = bd_mailbox.seq;
    if (op == BD_CLEAR) {
        memset(&bd_mailbox.card, 0, sizeof(bd_mailbox.card));
        bd_mailbox.card.progress = 255;
    }
    if (op == BD_CARD || op == BD_CLEAR) { bd_render(&bd_mailbox.card); ++bd_revision; }
    {
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        bd_pending = 0;
        OS_EXIT_CRITICAL();
    }
    bd_ack(version, op, seq, BD_OK, op == BD_HELLO);
}

static void display_back_cb(lv_event_t *e)
{
    (void)e;
    {
        OS_SR_ALLOC(); OS_ENTER_CRITICAL();
        g_badge_display_mode = 0; bd_pending = 0;
        OS_EXIT_CRITICAL();
    }
    ble_off();
    lv_scr_load(g_menu_scr); kp_set_menu();
}

static lv_obj_t *bd_label(int y, int height)
{
    lv_obj_t *label = lv_label_create(bd_screen);
    lv_obj_set_size(label, 240, height);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void display_open_cb(lv_event_t *e)
{
    bd_card *waiting = &bd_mailbox.card;
    (void)e;
    if (badge_ble_battery_gate()) return;
    if (!bd_screen) {
        bd_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(bd_screen, LV_OBJ_FLAG_SCROLLABLE);
        bd_title = bd_label(66, 72); /* bd_render below selects the title font. */
        bd_body = bd_label(148, 56); bd_footer = bd_label(246, 44);
        lv_obj_set_style_text_font(bd_body, bd_text_font(), 0);
        lv_obj_set_style_text_font(bd_footer, bd_text_font(), 0);
        /* Basic objects: upstream disables LV_USE_BAR to save code space. */
        bd_bar = lv_obj_create(bd_screen);
        lv_obj_remove_style_all(bd_bar);
        lv_obj_set_style_bg_opa(bd_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bd_bar, lv_color_hex(0x303840), 0);
        lv_obj_clear_flag(bd_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bd_bar, 220, 12);
        lv_obj_align(bd_bar, LV_ALIGN_TOP_MID, 0, 218);
        bd_fill = lv_obj_create(bd_bar);
        lv_obj_remove_style_all(bd_fill);
        lv_obj_set_style_bg_opa(bd_fill, LV_OPA_COVER, 0);
        lv_obj_set_size(bd_fill, 0, 12);
        bd_back = lv_btn_create(bd_screen);
        lv_obj_set_size(bd_back, 1, 1);
        lv_obj_set_style_opa(bd_back, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(bd_back, display_back_cb, LV_EVENT_SHORT_CLICKED, NULL);
    }
    /* Display mode is off here, so the BLE task cannot claim the mailbox: reuse it
     * as scratch for the waiting card instead of a ~370-byte stack copy. */
    memset(waiting, 0, sizeof(*waiting));
    waiting->background = 0x101820; waiting->foreground = 0xffffff;
    waiting->accent = 0x00c8a0; waiting->progress = 255;
    strcpy(waiting->title, "DISPLAY"); strcpy(waiting->body, "Waiting for PC\nDisplay protocol v1/v2");
    strcpy(waiting->footer, "Button 1 = back"); bd_render(waiting);
    g_badge_mouse_mode = 0;
    { extern void rcsp_hid_att_require_encryption(u8 on); rcsp_hid_att_require_encryption(0); }
    bd_pending = 0; g_badge_display_mode = 1;
    ble_on(); lv_scr_load(bd_screen); kp_set_one(bd_back);
}
