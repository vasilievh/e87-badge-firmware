/* Included in the LVGL task only. Keep charging power/radio handling conservative;
 * show status on the existing home/menu screens instead of a black charge page. */
#include "badge_power_status.h"
static badge_power_state badge_power;
static lv_obj_t *badge_home_power, *badge_menu_power;
static void badge_lock_wake(void);
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
    if (g_badge_locked || !badge_home_power || !badge_menu_power) return;
    color = badge_power_text(text, sizeof(text), get_vbat_percent(),
                             badge_usb_online(), get_charge_full_flag());
    if (color == badge_power_last_color && !strcmp(text, badge_power_last_text)) return;
    strcpy(badge_power_last_text, text); badge_power_last_color = color;
    lv_label_set_text(badge_home_power, text);
    lv_label_set_text(badge_menu_power, text);
    lv_obj_set_style_text_color(badge_home_power, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(badge_menu_power, lv_color_hex(color), 0);
}

static lv_obj_t *badge_power_label(lv_obj_t *parent, int home)
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
    lv_obj_align(label, home ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, home ? -32 : 40);
    return label;
}

static void badge_status_build(void)
{
    badge_home_power = badge_power_label(g_lock_scr, 1);
    badge_menu_power = badge_power_label(g_menu_scr, 0);
    badge_system_poll();
}

static void badge_charge_screen_enter(void)
{
    if (g_scanner_active) ble_op_scan_enable(0);
    g_scanner_active = g_scanner_browsing = g_scanner_detail_active = 0;
    g_badge_spoof_mode = g_badge_mouse_mode = g_badge_update_active = 0;
    g_on_update_scr = g_warn_active = g_ble_want_on = 0;
    ble_off();
    g_charge_active = 1;
    if (g_badge_locked) badge_lock_wake();
    lv_scr_load(g_menu_scr); kp_set_menu();
    /* Retain the upstream low-load charge policy, without hiding battery status. */
    extern int lcd_drv_backlight_ctrl(u8 percent);
    lcd_drv_backlight_ctrl(10);
}

static void badge_charge_screen_exit(void)
{
    g_charge_active = g_charge_waiting = 0;
    extern int lcd_drv_backlight_ctrl(u8 percent);
    lcd_drv_backlight_ctrl(g_bright_pct[g_bright_idx]);
}
