/* Compile the actual idle/UI headers against a deterministic OS/clock model.
 * This checks lost-wakeup ordering and telemetry, not real PMU/RTOS behavior. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef struct { unsigned count; } OS_SEM;
static unsigned create_fail, pend_calls, delay_calls, pend_ticks, delay_ticks;
static unsigned critical, replies;
static u8 wire[14];
static u32 now, sys_hz = 240000000;
static u16 mv = 4200;
static u8 percent = 100, usb, full, g_ble_want_on, g_badge_locked;
volatile u8 g_badge_adkey_kv = 0xff;
#define NO_KEY 0xff
#define OS_SR_ALLOC()
#define OS_ENTER_CRITICAL() do { assert(!critical); critical = 1; } while (0)
#define OS_EXIT_CRITICAL() do { assert(critical); critical = 0; } while (0)
#define TCFG_LOWPOWER_LOWPOWER_SEL 0
static int os_sem_create(OS_SEM *s, int value)
{ s->count = (unsigned)value; return create_fail ? -1 : 0; }
static int os_sem_post(OS_SEM *s) { ++s->count; return 0; }
static int os_sem_set(OS_SEM *s, u16 cnt) { assert(!critical); s->count = cnt; return 0; }
static int os_sem_pend(OS_SEM *s, int timeout)
{
    assert(!critical); ++pend_calls; pend_ticks = (unsigned)timeout;
    if (s->count) { --s->count; return 0; }
    return -1; /* Model a timeout/block; no wall-clock sleep in tests. */
}
static void os_time_dly(unsigned ticks)
{ assert(!critical); ++delay_calls; delay_ticks = ticks; }
static u32 timer_get_ms(void) { return now; }
u16 get_vbat_value(void) { assert(!critical); return mv; }
u8 get_vbat_percent(void) { assert(!critical); return percent; }
u8 get_charge_full_flag(void) { return full; }
static u8 badge_usb_online(void) { return usb; }
static u32 clk_get(const char *name) { assert(!strcmp(name, "sys")); return sys_hz; }
/* No clock_lock/clock_unlock model: the header must not call them (the SDK's
 * lcd_sleep_ctrl() owns the ui_show lock); a call would fail to compile here. */
void badge_power_reply(const u8 *p, u16 length)
{ assert(!critical && length == sizeof(wire)); memcpy(wire, p, length); ++replies; }
#include "badge_idle_ui.h"

static uint32_t le32(const u8 *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void)
{
    u8 request[] = {'B', 'P', 1, 0};
    u8 reply[BI_PAGE_SIZE];
    badge_idle_event touch = {0};
    badge_idle_init();
    assert(badge_key_event.ready && badge_idle.mv == 4200);
    assert(badge_idle.unlock_result == 127 && badge_idle.lock_result == 127);
    /* Awake key events do not fill the semaphore. A post before waiting is
     * retained, including one precisely between the caller's condition and pend. */
    badge_idle_key_changed(); assert(!badge_key_event.sem.count);
    now = 1000; badge_idle_prepare_lock(); g_badge_locked = 1; badge_idle_enter();
    g_badge_adkey_kv = 1; badge_idle_key_changed();
    g_badge_adkey_kv = NO_KEY; badge_idle_key_changed();
    assert(badge_key_event.sem.count == 1); /* release doesn't clear a pending press */
    badge_idle_wait(); assert(!badge_key_event.sem.count && pend_ticks == 100);
    assert(badge_idle_take_key_wake() && !badge_idle_take_key_wake());
    badge_idle_wait(); assert(pend_calls == 2 && badge_idle.waits == 2 && !delay_calls);
    /* Lock entry seeds the lowest locked clock with the clock at entry. */
    assert(badge_idle.idle_hz == 240000000);
    now += 1000; sys_hz = 24000000; badge_idle_sample(); /* SDK downclocks after panel sleep */
    assert(badge_idle.idle_hz == 24000000);
    /* Twelve-hour suspend, no percent or mV synthesis. Wake captures the SDK's
     * reading before display load; the SDK restores the clock in lcd_sleep_ctrl(0). */
    now = 43201000; percent = 11; mv = 3470;
    badge_idle_exit();
    assert(badge_idle.last_ms == 43200000 && badge_idle.exited_mv == 3470);
    assert(badge_idle.entered_mv == 4200 && badge_idle.entered_percent == 100);
    assert(!(badge_idle.flags & BI_LOCKED) && (badge_idle.flags & BI_LAST_VALID));
    sys_hz = 240000000; g_badge_locked = 0;
    badge_key_wake = 1; badge_idle_prepare_lock(); assert(!badge_idle_take_key_wake());
    /* Several presses in one lock leave several tokens; the next lock drains them,
     * so its first wait blocks instead of returning at once. */
    g_badge_locked = 1; g_badge_adkey_kv = 1;
    badge_idle_key_changed(); badge_idle_key_changed(); badge_idle_key_changed();
    g_badge_adkey_kv = NO_KEY; g_badge_locked = 0;
    assert(badge_key_event.sem.count == 3);
    badge_idle_prepare_lock(); assert(!badge_key_event.sem.count);
    badge_key_wake = 0;
    g_ble_want_on = usb = full = 1; badge_idle_sample();
    for (unsigned page = 0; page < 4; ++page) {
        request[3] = (u8)page;
        badge_idle_status before = badge_idle;
        assert(badge_power_command(BI_CMD, request, sizeof(request)) == 1);
        assert(memcmp(&badge_idle, &before, sizeof(before)) == 0);
        assert(memcmp(wire, request, 4) == 0);
        if (page == 0) { assert(wire[4] == 30 && wire[5] == 11 && wire[12] == 0); }
        if (page == 1) { assert(wire[4] == 100 && wire[5] == 11 && le32(wire + 10) == 43200); }
        if (page == 2) { assert(le32(wire + 4) == 0 && le32(wire + 8) == 2); }
        if (page == 3) { assert(le32(wire + 4) == 240000000 && le32(wire + 8) == 24000000); }
    }
    assert(replies == 4);
    assert(!badge_power_command(0xc0, request, 4)); /* no OTA handling */
    request[3] = 4; assert(badge_power_command(BI_CMD, request, 4));
    request[3] = 0; request[2] = 2; badge_power_command(BI_CMD, request, 4);
    request[2] = 1;
    for (unsigned len = 0; len < 4; ++len) badge_power_command(BI_CMD, request, (u16)len);
    assert(replies == 4);
    /* Bounds: invalid packets never touch the output, including length overflow. */
    for (unsigned len = 0; len < 100; ++len) {
        memset(reply, 0xa5, sizeof(reply));
        int size = bi_page(&badge_idle, request, len, reply);
        assert((len == 4) == !!size);
        if (!size) for (unsigned i = 0; i < sizeof(reply); ++i) assert(reply[i] == 0xa5);
    }
    /* A new in-progress lock must not mix its start with the previous interval's end. */
    badge_idle_enter(); request[3] = 1;
    assert(bi_page(&badge_idle, request, 4, reply) == BI_PAGE_SIZE);
    assert(reply[4] == 100 && reply[5] == 11 && le32(reply + 10) == 43200);
    /* Timer wrap is handled by unsigned subtraction (sleep < 2^32 ms). */
    badge_idle_status wrapping = {0};
    wrapping.now_ms = 0xfffff000; bi_enter(&wrapping);
    wrapping.now_ms += 60000; bi_exit(&wrapping); assert(wrapping.last_ms == 60000);
    /* Clock API result fields stay "not attempted". */
    assert(badge_idle.unlock_result == 127 && badge_idle.lock_result == 127);
    /* Touch waits indefinitely while gated off, and enable posts are retained.
     * Repeated off/on cycles do not leak a semaphore allocation per cycle. */
    badge_idle_event_init(&touch);
    for (unsigned i = 0; i < 100; ++i) {
        badge_idle_event_signal(&touch); badge_idle_event_wait(&touch, 0);
        assert(!touch.sem.count && pend_ticks == 0);
    }
    create_fail = 1; badge_idle_event_init(&touch);
    badge_idle_event_signal(&touch); badge_idle_event_wait(&touch, 0);
    assert(!touch.ready && delay_ticks == 10);
    badge_idle_event_wait(&touch, 100); assert(delay_ticks == 100);
    puts("Idle: retained key/touch wakes, timeout/fallback, SDK-owned clock, telemetry, bounds and timer wrap OK");
    return 0;
}
