/* Included in the UI translation unit after the BLE helpers and state flags.
 * Only the UI's own clock-manager lock is released/restored; no raw clock/PMU or
 * charging writes. Only this task updates the cached measurements.
 * BLE reads/encodes the bounded snapshot under the same critical section. */
#include "badge_idle_event.h"
#include "badge_idle_status.h"
static badge_idle_event badge_key_event;
static badge_idle_status badge_idle;
static volatile u8 badge_key_wake;
extern volatile u8 g_badge_adkey_kv;
extern void badge_power_reply(const u8 *data, u16 len);
extern int clock_unlock(char *name);

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
    OS_SR_ALLOC(); OS_ENTER_CRITICAL(); badge_key_wake = 0; OS_EXIT_CRITICAL();
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
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL(); bi_enter(&badge_idle); OS_EXIT_CRITICAL(); }
}
static void badge_idle_exit(void)
{
    /* Capture the cached battery reading BEFORE powering the LCD back up. */
    badge_idle_sample();
    { OS_SR_ALLOC(); OS_ENTER_CRITICAL(); bi_exit(&badge_idle); OS_EXIT_CRITICAL(); }
}
static void badge_idle_release_clock(void)
{
    /* Called only AFTER lcd_sleep_ctrl(1). The manager respects other users and
     * rejects an unlock if ui_show is not the current owner; never force a clock.
     * Its adaptive calibration takes several 4 s steps; the immediate frequency
     * may still be maximum. Sampling records the lowest observed locked clock.
     * With no other requirements the manager's floor is 24 MHz. */
    int result = clock_unlock("ui_show");
    u32 hz = clk_get("sys");
    OS_SR_ALLOC(); OS_ENTER_CRITICAL();
    badge_idle.unlock_result = (int8_t)result;
    badge_idle.idle_hz = badge_idle.sys_hz = hz;
    OS_EXIT_CRITICAL();
}
static void badge_idle_restore_clock(void)
{
    /* Restore BEFORE LCD re-init and GPU rendering, through the same SDK API
     * used at UI startup. Do not steal a lock from USB or another clock user. */
    int result = clock_lock("ui_show", clk_get_max_frequency());
    OS_SR_ALLOC(); OS_ENTER_CRITICAL();
    badge_idle.lock_result = (int8_t)result;
    OS_EXIT_CRITICAL();
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
