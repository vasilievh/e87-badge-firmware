/* Included in lvgl_ui.c after ble_on/off, kp_set_one/menu and the battery gate.
 * Only the UI task calls LVGL. BLE RX posts one bounded request to a mailbox. */
#include "badge_display_protocol.h"
volatile u8 g_badge_display_mode;
static volatile u8 bd_pending;
static bd_request bd_mailbox;
static uint16_t bd_revision;
static lv_obj_t *bd_screen, *bd_title, *bd_body, *bd_footer, *bd_bar, *bd_fill, *bd_back;
extern void badge_display_reply(const u8 *data, u16 len);

static void bd_ack(uint8_t op, uint16_t seq, uint8_t status, int hello)
{
    uint8_t p[14] = {'B', 'D', BD_VERSION, (uint8_t)(op | 0x80),
                    (uint8_t)seq, (uint8_t)(seq >> 8), status};
    p[7] = (uint8_t)bd_revision; p[8] = (uint8_t)(bd_revision >> 8);
    if (hello && status == BD_OK) {
        p[7] = 0x68; p[8] = 1; p[9] = 0x68; p[10] = 1; /* 360 x 360 LE */
        p[11] = BD_TITLE_MAX; p[12] = BD_BODY_MAX; p[13] = BD_FOOTER_MAX;
    }
    /* Full HELLO frame is exactly 20 bytes, even at the default ATT MTU 23. */
    badge_display_reply(p, hello && status == BD_OK ? 14 : 9);
}

/* Called by the BLE task, never by LVGL. Decode first, then publish atomically. */
int badge_display_command(u8 cmd, const u8 *p, u16 n)
{
    bd_request next;
    int status;
    uint8_t op;
    uint16_t seq;
    if (cmd != BD_CMD) return 0;
    /* Do not answer unrelated vendor 0x90 payloads. */
    if (n < 6 || p[0] != 'B' || p[1] != 'D') return 1;
    op = p[3]; seq = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    status = bd_decode(p, n, &next);
    if (status == BD_OK) {
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        if (!g_badge_display_mode) status = BD_INACTIVE;
        else if (bd_pending) status = BD_BUSY;
        else { bd_mailbox = next; bd_pending = 1; }
        OS_EXIT_CRITICAL();
    }
    if (status != BD_OK) bd_ack(op, seq, status, 0);
    return 1;
}

static void bd_render(const bd_card *c)
{
    lv_obj_t *labels[3] = {bd_title, bd_body, bd_footer};
    int i;
    /* Upstream strips the 36px font to ASCII 32..90. Fall back to the complete
     * 14px ASCII font for lowercase/punctuation rather than losing glyphs. */
    int large = 1;
    for (i = 0; c->title[i]; ++i) if ((unsigned char)c->title[i] > 90) large = 0;
    lv_obj_set_style_text_font(bd_title, large ? &lv_font_montserrat_36 : &lv_font_montserrat_14, 0);
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
    bd_request request;
    int ready = 0;
    {
        OS_SR_ALLOC();
        OS_ENTER_CRITICAL();
        if (g_badge_display_mode && bd_pending) {
            request = bd_mailbox; bd_pending = 0; ready = 1;
        }
        OS_EXIT_CRITICAL();
    }
    if (!ready) return;
    if (request.op == BD_CARD) { bd_render(&request.card); ++bd_revision; }
    else if (request.op == BD_CLEAR) {
        bd_card blank;
        memset(&blank, 0, sizeof(blank)); blank.progress = 255;
        bd_render(&blank); ++bd_revision;
    }
    bd_ack(request.op, request.seq, BD_OK, request.op == BD_HELLO);
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
    bd_card waiting;
    (void)e;
    if (badge_ble_battery_gate()) return;
    if (!bd_screen) {
        bd_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(bd_screen, LV_OBJ_FLAG_SCROLLABLE);
        bd_title = bd_label(66, 72);
        lv_obj_set_style_text_font(bd_title, &lv_font_montserrat_36, 0);
        bd_body = bd_label(148, 56); bd_footer = bd_label(246, 44);
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
    memset(&waiting, 0, sizeof(waiting));
    waiting.background = 0x101820; waiting.foreground = 0xffffff;
    waiting.accent = 0x00c8a0; waiting.progress = 255;
    strcpy(waiting.title, "DISPLAY"); strcpy(waiting.body, "Waiting for PC\nEC-BADGE / protocol v1");
    strcpy(waiting.footer, "Button 1 = back"); bd_render(&waiting);
    g_badge_mouse_mode = 0;
    { extern void rcsp_hid_att_require_encryption(u8 on); rcsp_hid_att_require_encryption(0); }
    bd_pending = 0; g_badge_display_mode = 1;
    ble_on(); lv_scr_load(bd_screen); kp_set_one(bd_back);
}
