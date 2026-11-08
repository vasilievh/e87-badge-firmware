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
    card[2] = 2; assert(bd_decode(card,sizeof(card),&out) == BD_BAD_VERSION); card[2]=1;
    /* Deterministic malformed-input fuzz: no state changes on rejection. */
    srand(87);
    for (i=0; i<20000; ++i) {
        uint8_t fuzz[260];
        n = (unsigned)rand() % sizeof(fuzz);
        for (unsigned j=0; j<n; ++j) fuzz[j]=(uint8_t)rand();
        if (n >= 6 && i % 2 == 0) {
            fuzz[0]='B'; fuzz[1]='D'; fuzz[2]=1; fuzz[3]=(uint8_t)(1 + i % 5);
        }
        out=before;
        if (bd_decode(fuzz,n,&out) != BD_OK) assert(memcmp(&out,&before,sizeof(out)) == 0);
    }
    badge_display_command(0x90,hello,sizeof(hello));
    assert(response[6] == BD_INACTIVE && allocations == 0);
    display_open_cb(NULL); assert(radio && g_badge_display_mode);
    ui_calls=0;
    badge_display_command(0x90,hello,sizeof(hello));
    assert(ui_calls == 0 && bd_pending); /* BLE thread must never call LVGL */
    badge_display_command(0x90,hello,sizeof(hello)); assert(response[6] == BD_BUSY);
    badge_display_tick(); assert(response_len == 14 && response[6] == BD_OK && depth == 0);
    badge_display_command(0x90,card,sizeof(card)); assert(ui_calls == 0);
    badge_display_tick(); assert(ui_calls > 0 && response[6] == BD_OK && bd_revision == 1);
    display_back_cb(NULL); assert(!radio && !g_badge_display_mode && !bd_pending);
    display_open_cb(NULL); assert(allocations == 7); /* screen reused, no new objects */
    puts("display native: bounds, fuzz, mailbox, acknowledgements, lifecycle OK");
    return 0;
}
