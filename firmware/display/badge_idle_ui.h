/* Included in the UI translation unit after the BLE helpers and state flags.
 * No clock, PMU or charging writes: lcd_sleep_ctrl() itself releases the UI's
 * "ui_show" clock lock on panel sleep and re-takes it on wake; this file only
 * samples the resulting frequency. Only this task updates the cached measurements.
 * BLE reads/encodes the bounded snapshot under the same critical section. */
#include "badge_idle_event.h"
#include "badge_idle_status.h"
static badge_idle_event badge_key_event;
static badge_idle_status badge_idle;
static volatile u8 badge_key_wake;
extern volatile u8 g_badge_adkey_kv;
extern void badge_power_reply(const u8 *data, u16 len);

static void badge_idle_sample(void)
{
    extern u16 get_vbat_value(void);
    extern u8 get_vbat_percent(void);
    extern u8 get_charge_full_flag(void);
    u32 now = timer_get_ms();
    u16 mv = get_vbat_value();
    u8 percent = get_vbat_percent();
    u32 hz = clk_get("sys");
    u8 flags = (badge_usb_online() ? BI_USB : 0) |
               (get_charge_full_flag() ? BI_FULL : 0) |
               (g_ble_want_on ? BI_BLE_WANTED : 0);
    OS_SR_ALLOC(); OS_ENTER_CRITICAL();
    badge_idle.now_ms = now; badge_idle.mv = mv; badge_idle.percent = percent;
    badge_idle.sys_hz = hz;
    if ((badge_idle.flags & BI_LOCKED) && hz < badge_idle.idle_hz) badge_idle.idle_hz = hz;
    badge_idle.flags = (badge_idle.flags & (BI_LOCKED | BI_LAST_VALID)) | flags;
    badge_idle.lowpower = TCFG_LOWPOWER_LOWPOWER_SEL;
    OS_EXIT_CRITICAL();
}
static void badge_idle_init(void)
{
    badge_idle_event_init(&badge_key_event);
    badge_idle.unlock_result = badge_idle.lock_result = 127; /* not attempted */
    badge_idle_sample();
}
/* Called on a debounced key transition, possibly from the scan timer. Never
 * reads flash, logs, renders or blocks. No posts accumulate while the UI is awake. */
void badge_idle_key_changed(void)
{
    if (g_badge_locked && g_badge_adkey_kv != NO_KEY) {
        badge_key_wake = 1; /* retain even a press+release before the UI runs */
        badge_idle_event_signal(&badge_key_event);
    }
}
static void badge_idle_prepare_lock(void)
{
    /* Called BEFORE g_badge_locked=1. Ignore the key that entered Lock and any
     * stale notification from a previous LCD re-init; new presses are retained. */
    badge_idle_event_clear(&badge_key_event); /* nobody posts while unlocked */
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL(); badge_key_wake = 0; OS_EXIT_CRITICAL(); }
}
static int badge_idle_take_key_wake(void)
{
    int wake;
    OS_SR_ALLOC(); OS_ENTER_CRITICAL();
    wake = badge_key_wake; badge_key_wake = 0;
    OS_EXIT_CRITICAL();
    return wake;
}
static void badge_idle_enter(void)
{
    badge_idle_sample();
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL();
      bi_enter(&badge_idle);
      badge_idle.idle_hz = badge_idle.sys_hz; /* lowered by later locked samples */
      OS_EXIT_CRITICAL(); }
}
static void badge_idle_exit(void)
{
    /* Capture the cached battery reading BEFORE powering the LCD back up. */
    badge_idle_sample();
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL(); bi_exit(&badge_idle); OS_EXIT_CRITICAL(); }
}
static void badge_idle_wait(void)
{
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL(); ++badge_idle.waits; OS_EXIT_CRITICAL(); }
    /* Key transitions wake immediately; timeout services USB/power once a second.
     * ADKEY's original 10 ms scanner remains intact, so short taps aren't lost. */
    badge_idle_event_wait(&badge_key_event, 100);
}
/* Read-only 0x92 is dispatched in the BLE task before Display/Update gates.
 * Opening a BLE tool is required; diagnostics never enable the idle radio. */
int badge_power_command(u8 cmd, const u8 *p, u16 n)
{
    u8 reply[BI_PAGE_SIZE];
    int size;
    if (cmd != BI_CMD) return 0;
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL();
      size = bi_page(&badge_idle, p, n, reply);
      OS_EXIT_CRITICAL(); }
    if (size) badge_power_reply(reply, (u16)size);
    return 1;
}
