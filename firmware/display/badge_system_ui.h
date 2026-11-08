/* Included in the LVGL task only. USB changes status, never the active tool,
 * radio, screen or saved brightness. The upstream OTA-session dimmer is separate. */
#include "badge_power_status.h"
static badge_power_state badge_power;
static lv_obj_t *badge_menu_power;
static u32 badge_power_poll_ms;
static char badge_power_last_text[24];
static unsigned badge_power_last_color;

static u8 badge_usb_online(void)
{
    return badge_power.online;
}

static void badge_system_poll(void)
{
    extern u8 get_charge_online_flag(void);
    extern u8 get_ldo5v_online_hw(void);
    extern u8 get_lvcmp_det(void);
    extern u8 get_charge_full_flag(void);
    extern u8 get_vbat_percent(void);
    u32 now = timer_get_ms();
    char text[24];
    unsigned color;
    if (badge_power.initialized && (u32)(now - badge_power_poll_ms) < 100) return;
    badge_power_poll_ms = now;
    /* Read hardware presence as well as the SDK's event-maintained flag. The
     * application flag alone was not sufficient to show charge UI on all units. */
    badge_power_sample(&badge_power, get_charge_online_flag() ||
                       get_ldo5v_online_hw() || get_lvcmp_det());
    if (g_badge_locked || !badge_menu_power) return;
    color = badge_power_text(text, sizeof(text), get_vbat_percent(),
                             badge_usb_online(), get_charge_full_flag());
    if (color == badge_power_last_color && !strcmp(text, badge_power_last_text)) return;
    strcpy(badge_power_last_text, text); badge_power_last_color = color;
    lv_label_set_text(badge_menu_power, text);
    lv_obj_set_style_text_color(badge_menu_power, lv_color_hex(color), 0);
}

static lv_obj_t *badge_power_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, 204);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 4, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 40);
    return label;
}

static void badge_status_build(void)
{
    badge_menu_power = badge_power_label(g_menu_scr);
    badge_system_poll();
}

static void badge_charge_screen_enter(void)
{
    g_charge_active = 1;
}

static void badge_charge_screen_exit(void)
{
    g_charge_active = g_charge_waiting = 0;
}
