/* badge_safety.h -- E87 safety net. RAM only: nothing here writes flash.
 *
 * 1. Suspend TE guard. lcd_drv_power_off() (SDK lcd_drive.c) powers the panel
 *    down and leaves its TE pin (TCFG_LCD_TE_IO, PC00) in PORT_HIGHZ, while the
 *    falling-edge GPIO interrupt armed by lcd_te_irq_init() (TCFG_LCD_TE_USED_PEND)
 *    stays enabled. With the panel unpowered that input floats, and every noise
 *    edge is an interrupt. An LCD sleep handler (run by lcd_sleep_ctrl() after the
 *    power-off) pulls the pin down; lcd_init_task() restores its 10K pull-up when
 *    the panel is powered again on wake.
 * 2. TE interrupt counter. The LVGL build calls the SDK's empty weak hook
 *    lv_lcd_te_sync_int_handler() from the TE interrupt; a strong definition here
 *    counts calls. Nothing else defines it (gpu_task.c defines the separate
 *    lcd_te_sync_int_handler, which is left alone).
 * 3. UI liveness watchdog. The LVGL main loop ticks a counter. A hardware-timer
 *    callback resets the CPU when the loop has made fewer than
 *    BS_WD_MIN_LOOPS_PER_10S passes per 10 s of wall time for BS_WD_STALL_MS.
 *    The loop normally runs ~100 times/s (~33/s while suspended, ~1/s with the
 *    fix/lock-power wait). It is exempt while the Update screen is open or an
 *    update is active. Holding a button for over a minute inside a "wait for
 *    release" loop also stalls the UI loop and therefore resets the badge.
 *
 * Read-only diagnostics over Qix (BLE on, e.g. on the Update screen):
 *   request 0x97 'B' 'S' 1 page(0..2) -> reply 0x98, 14 bytes:
 *   page 0: te_irqs u32, ui_loops u32, flags u8 (1 asleep, 2 last sleep valid,
 *           4 watchdog exempt), stalled seconds u8 (capped at 255)
 *   page 1: TE interrupts during the last panel sleep u32, its length in s u32
 *   page 2: TE interrupts in the current sleep u32, its length so far in s u32
 */
#ifndef BADGE_SAFETY_H
#define BADGE_SAFETY_H
#include <stdint.h>
#include <string.h>

#define BS_CMD 0x97
#define BS_REPLY 0x98
#define BS_PAGE_SIZE 14
#define BS_WD_PERIOD_MS 5000u
#define BS_WD_MIN_LOOPS_PER_10S 5u
#define BS_WD_STALL_MS 60000u
#ifndef BADGE_SAFETY_WATCHDOG
#define BADGE_SAFETY_WATCHDOG 1   /* 0 = keep counting, never reset */
#endif

#define BS_ASLEEP 1
#define BS_LAST_VALID 2
#define BS_EXEMPT 4

typedef struct {
    uint32_t last_ms, last_loops, stalled_ms;
    uint8_t armed, exempt;
} bs_watchdog;

/* Returns 1 when the CPU should be reset. Wall-time based, so a timer that is
 * itself delayed still sees how little the UI loop advanced in between. */
static int bs_watchdog_check(bs_watchdog *w, uint32_t now_ms, uint32_t loops, int exempt)
{
    uint32_t elapsed, progressed;
    w->exempt = exempt ? 1 : 0;
    if (!w->armed || exempt) {
        w->armed = 1;
        w->last_ms = now_ms;
        w->last_loops = loops;
        w->stalled_ms = 0;
        return 0;
    }
    elapsed = now_ms - w->last_ms;
    progressed = loops - w->last_loops;
    w->last_ms = now_ms;
    w->last_loops = loops;
    if ((uint64_t)progressed * 10000u < (uint64_t)BS_WD_MIN_LOOPS_PER_10S * elapsed) {
        w->stalled_ms = (w->stalled_ms + elapsed < w->stalled_ms) ? 0xFFFFFFFFu
                                                                  : w->stalled_ms + elapsed;
    } else {
        w->stalled_ms = 0;
    }
    return w->stalled_ms >= BS_WD_STALL_MS;
}

typedef struct {
    uint32_t sleep_ms, sleep_te;          /* start of the current sleep */
    uint32_t last_ms, last_te;            /* length / TE count of the last sleep */
    uint8_t flags;
} bs_sleep;

static void bs_sleep_enter(bs_sleep *s, uint32_t now_ms, uint32_t te)
{
    if (s->flags & BS_ASLEEP) return;
    s->sleep_ms = now_ms;
    s->sleep_te = te;
    s->flags |= BS_ASLEEP;
}

static void bs_sleep_exit(bs_sleep *s, uint32_t now_ms, uint32_t te)
{
    if (!(s->flags & BS_ASLEEP)) return;
    s->last_ms = now_ms - s->sleep_ms;
    s->last_te = te - s->sleep_te;
    s->flags = (uint8_t)((s->flags & ~BS_ASLEEP) | BS_LAST_VALID);
}

static void bs_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Fills a 14-byte reply for a valid request; returns its length or 0. */
static int bs_page(const bs_sleep *s, const bs_watchdog *w, uint32_t te, uint32_t loops,
                   uint32_t now_ms, const uint8_t *request, unsigned length, uint8_t *reply)
{
    unsigned page;
    uint32_t stalled_s;
    if (length != 4 || request[0] != 'B' || request[1] != 'S' || request[2] != 1
        || request[3] > 2) return 0;
    page = request[3];
    memset(reply, 0, BS_PAGE_SIZE);
    memcpy(reply, request, 4);
    if (page == 0) {
        stalled_s = w->stalled_ms / 1000u;
        bs_u32(reply + 4, te);
        bs_u32(reply + 8, loops);
        reply[12] = (uint8_t)((s->flags & (BS_ASLEEP | BS_LAST_VALID)) | (w->exempt ? BS_EXEMPT : 0));
        reply[13] = (uint8_t)(stalled_s > 255u ? 255u : stalled_s);
    } else if (page == 1) {
        if (s->flags & BS_LAST_VALID) {
            bs_u32(reply + 4, s->last_te);
            bs_u32(reply + 8, s->last_ms / 1000u);
        }
    } else if (s->flags & BS_ASLEEP) {
        bs_u32(reply + 4, te - s->sleep_te);
        bs_u32(reply + 8, (now_ms - s->sleep_ms) / 1000u);
    }
    return BS_PAGE_SIZE;
}

#ifdef BADGE_SAFETY_IMPLEMENTATION
/* SDK glue: included once, by lvgl_main.c, after g_on_update_scr and
 * g_badge_update_active are defined. */
#include "gpio.h"
#include "system/timer.h"
#include "asm/cpu.h"
#include "ui/lcd/lcd_drive.h"

static volatile uint32_t bs_te_irqs;
static volatile uint32_t bs_ui_loops;
static bs_sleep bs_sleep_state;
static bs_watchdog bs_wd;

/* TE interrupt context: count only. */
void lv_lcd_te_sync_int_handler(void)
{
    bs_te_irqs++;
}

static void bs_lcd_sleep_enter(void)
{
#if defined(TCFG_LCD_TE_USED_PEND) && TCFG_LCD_TE_USED_PEND && defined(TCFG_LCD_TE_IO)
    if (TCFG_LCD_TE_IO != NO_CONFIG_PORT) {
        gpio_set_mode(IO_PORT_SPILT(TCFG_LCD_TE_IO), PORT_INPUT_PULLDOWN_10K);
    }
#endif
    bs_sleep_enter(&bs_sleep_state, timer_get_ms(), bs_te_irqs);
}

static void bs_lcd_sleep_exit(void)
{
    bs_sleep_exit(&bs_sleep_state, timer_get_ms(), bs_te_irqs);
}

REGISTER_LCD_SLEEP_HEADLER(badge_safety_lcd_sleep) = {
    .name = "badge_safety",
    .enter = bs_lcd_sleep_enter,
    .exit = bs_lcd_sleep_exit,
};

static inline void badge_safety_ui_tick(void)
{
    bs_ui_loops++;
}

/* Hardware-timer context (same timer class as the ADKEY scan), so it still runs
 * when tasks are starved. cpu_reset() is the same reset RAW_REBOOT uses. */
static void bs_watchdog_timer(void *priv)
{
    (void)priv;
    if (bs_watchdog_check(&bs_wd, timer_get_ms(), bs_ui_loops,
                          g_badge_update_active || g_on_update_scr)) {
#if BADGE_SAFETY_WATCHDOG
        cpu_reset();
#endif
    }
}

static void badge_safety_start(void)
{
    sys_hi_timer_add(NULL, bs_watchdog_timer, BS_WD_PERIOD_MS);
}

/* BLE task: read-only snapshot of the counters. */
void badge_safety_reply(const u8 *data, u16 len);
int badge_safety_command(u8 cmd, const u8 *p, u16 n)
{
    uint8_t reply[BS_PAGE_SIZE];
    int length;
    if (cmd != BS_CMD) return 0;
    length = bs_page(&bs_sleep_state, &bs_wd, bs_te_irqs, bs_ui_loops, timer_get_ms(),
                     p, n, reply);
    if (length) badge_safety_reply(reply, (u16)length);
    return 1;
}
#endif
#endif
