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
 *    callback samples it every BS_WD_PERIOD_MS and resets the CPU when, over the
 *    last BS_WD_STALL_MS of wall time, the loop made fewer than
 *    BS_WD_MIN_LOOPS_PER_MIN passes per minute. A sliding window, so short bursts of
 *    activity inside a long stall do not hide it. The loop normally runs ~100
 *    times/s (~33/s while suspended, ~1/s with the fix/lock-power wait). It starts
 *    in jl_gui_init(), before the boot-time screen build and suspend, and is exempt
 *    while the Update screen is open or an update is active. Holding a button
 *    inside a "wait for release" loop also stalls the UI loop: about 30 s from the
 *    lock screen, about 60 s when awake, then the badge resets.
 *    Reset: interrupts off, then spin, so the SDK's hardware watchdog
 *    (wdt_init(WDT_APP_RUN_TIME) = WDT_LRC_4S in app_main.c, WDT_16S during setup;
 *    checked by prepare-badge-system.py) resets the chip, as the SDK's own ASSERT
 *    does in this debug-UART build. No PMU access from interrupt context unless
 *    that watchdog has failed to fire (then cpu_reset() as a last resort).
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
#define BS_WD_MIN_LOOPS_PER_MIN 30u   /* 0.5 passes/s over the last minute */
#define BS_WD_STALL_MS 60000u
#define BS_WD_SAMPLES 13u   /* 12 periods of BS_WD_PERIOD_MS = BS_WD_STALL_MS */
#ifndef BADGE_SAFETY_WATCHDOG
#define BADGE_SAFETY_WATCHDOG 1   /* 0 = keep counting, never reset */
#endif

#define BS_ASLEEP 1
#define BS_LAST_VALID 2
#define BS_EXEMPT 4

typedef struct {
    uint32_t ms[BS_WD_SAMPLES], loops[BS_WD_SAMPLES];
    uint32_t stalled_ms;          /* length of the current low-progress window */
    uint8_t count, next, exempt;
} bs_watchdog;

/* Returns 1 when the CPU should be reset. Compares the newest sample with the
 * most recent stored sample that is at least BS_WD_STALL_MS old, i.e. the progress
 * over (just over) the last minute. Wall-time based, so a timer that is itself
 * delayed still sees how little the UI loop advanced in between. */
static int bs_watchdog_check(bs_watchdog *w, uint32_t now_ms, uint32_t loops, int exempt)
{
    unsigned k, i;
    uint32_t span, progressed;
    w->exempt = exempt ? 1 : 0;
    w->stalled_ms = 0;
    if (exempt) {
        w->count = 0;
        w->next = 0;
        return 0;
    }
    w->ms[w->next] = now_ms;
    w->loops[w->next] = loops;
    w->next = (uint8_t)((w->next + 1u) % BS_WD_SAMPLES);
    if (w->count < BS_WD_SAMPLES) w->count++;
    for (k = 1; k < w->count; k++) {
        i = (w->next + BS_WD_SAMPLES - 1u - k) % BS_WD_SAMPLES;
        span = now_ms - w->ms[i];
        if (span < BS_WD_STALL_MS) continue;
        progressed = loops - w->loops[i];
        if ((uint64_t)progressed * 60000u < (uint64_t)BS_WD_MIN_LOOPS_PER_MIN * span)
            w->stalled_ms = span;
        return w->stalled_ms != 0;
    }
    return 0;   /* less than a minute of history yet */
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

/* Not "ui/lcd/lcd_drive.h": it pulls app_config.h into lvgl_main.c for the first
 * time (redefining TCFG_UI_ENABLE and the macro defaults for everything after this
 * point). Same layout and section as the SDK's REGISTER_LCD_SLEEP_HEADLER(), which
 * interface/ui/ui.ld collects with KEEP(*(.lcd_sleep_headler)). The TE pin and
 * TCFG_LCD_TE_USED_PEND are checked against the SDK by prepare-badge-system.py. */
struct lcd_sleep_headler {
    char *name;
    void (*enter)(void);
    void (*exit)(void);
};
#define BS_TE_PIN IO_PORTC_00   /* TCFG_LCD_PIN_TE in sdk_config.h */

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
    gpio_set_mode(IO_PORT_SPILT(BS_TE_PIN), PORT_INPUT_PULLDOWN_10K);
    bs_sleep_enter(&bs_sleep_state, timer_get_ms(), bs_te_irqs);
}

static void bs_lcd_sleep_exit(void)
{
    bs_sleep_exit(&bs_sleep_state, timer_get_ms(), bs_te_irqs);
}

const struct lcd_sleep_headler badge_safety_lcd_sleep
    __attribute__((section(".lcd_sleep_headler"), used)) = {
    .name = "badge_safety",
    .enter = bs_lcd_sleep_enter,
    .exit = bs_lcd_sleep_exit,
};

static inline void badge_safety_ui_tick(void)
{
    bs_ui_loops++;
}

#define BS_RESET_FALLBACK_SPINS 0x40000000u   /* tens of s at 240 MHz, minutes at 24 MHz */
/* SDK reference (cpu/br35/setup.c, cpu_assert_debug):
 *   CONFIG_DEBUG_ENABLE (TCFG_DEBUG_UART_ENABLE 1, this build): local_irq_disable(); while (1);
 *   release build: P3_PCNT_SET0 = 0xac; cpu_reset();   (cpu_reset() = p33_soft_reset())
 * We take the first: with interrupts off nothing clears the hardware watchdog, which
 * then resets the chip. p33_soft_reset() is a PMU (P33) access and could interleave
 * with a P33 transfer of the interrupted task, so it is only the fallback for a
 * watchdog that did not fire. */
static void bs_reset(void)
{
    volatile uint32_t spin;
    local_irq_disable();
    for (spin = 0; spin < BS_RESET_FALLBACK_SPINS; spin++) { }
    cpu_reset();
    for (;;) { }
}

/* Hardware-timer context (same timer class as the ADKEY scan), so it still runs
 * when tasks are starved. */
static void bs_watchdog_timer(void *priv)
{
    (void)priv;
    if (bs_watchdog_check(&bs_wd, timer_get_ms(), bs_ui_loops,
                          g_badge_update_active || g_on_update_scr)) {
#if BADGE_SAFETY_WATCHDOG
        bs_reset();
#endif
    }
}

static void badge_safety_start(void)
{
    static uint8_t started;
    if (started) return;
    started = 1;
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
