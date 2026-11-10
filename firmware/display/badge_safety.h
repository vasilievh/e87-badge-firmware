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
 *    lock screen, about 60 s when awake, then the badge blinks (item 4) and resets.
 *    The check also decides when the history is one sample short of a full
 *    minute (callbacks a few ms faster than BS_WD_PERIOD_MS by timer_get_ms(); the
 *    old rule then never decided -- docs/hypotheses.md, 2026-11-10) and when the
 *    loop made no progress at all over the full history, whatever timer_get_ms()
 *    says the span was.
 * 4. Stage marks. The sleep/wake path and the draw call record which step they
 *    are in (BS_STAGE_*). On a stall the watchdog first blinks the backlight:
 *    1.5 s dark, then one short pulse per stage number, three rounds (at most
 *    ~25 s), then resets. A separate 250 ms timer drives the blinks; if it never
 *    runs, the watchdog resets anyway after BS_BLINK_MAX_WD_CALLS periods.
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
 *   page 3: watchdog timer calls u32, stage u8, flags u8 (1 blinking), last
 *           callback interval by timer_get_ms() u16 ms, by jiffies u16 ms
 *           (both capped at 65535)
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

/* Where the UI task is; the blink count before a watchdog reset. */
#define BS_STAGE_AWAKE        1   /* awake, outside the steps below */
#define BS_STAGE_SLEEP_ENTER  2   /* lcd_sleep_ctrl(1): backlight off, panel to sleep */
#define BS_STAGE_ASLEEP       3   /* sleep entry finished; idle wait */
#define BS_STAGE_WAKE_POWER   4   /* lcd_sleep_ctrl(0): panel power and re-init start */
#define BS_STAGE_WAKE_REINIT  5   /* lcd_reinit_wait_finish(): waiting for the re-init task */
#define BS_STAGE_WAKE_DONE    6   /* re-init done: backlight, touch, waiting for key release */
#define BS_STAGE_DRAW         7   /* lv_timer_handler() in the menu loop */
#define BS_STAGE_ASLEEP_DRAW  8   /* an LVGL pass with the panel asleep: at boot after the
                                     boot-time sleep, or right after a Button 1 suspend */
#define BS_STAGE_BOOT         9   /* boot: before the first sleep */

#define BS_BLINK_TICK_MS      250u
#define BS_BLINK_GAP_TICKS    6u    /* 1.5 s dark before each round */
#define BS_BLINK_ON_TICKS     1u
#define BS_BLINK_OFF_TICKS    2u
#define BS_BLINK_ROUNDS       3u
#define BS_BLINK_LEVEL        80u   /* backlight percent of a pulse */
#define BS_BLINK_MAX_WD_CALLS 8u    /* 40 s: longer than any blink sequence */

typedef struct {
    uint32_t calls;               /* watchdog timer callbacks since boot */
    uint16_t interval_ms;         /* last callback interval by timer_get_ms() */
    uint16_t interval_jiffies_ms; /* the same interval by jiffies */
    uint8_t stage, blinking;
} bs_diag;

/* Backlight percent for tick `tick` of a sequence of `pulses` pulses per round,
 * or -1 once the sequence is over. Stage numbers outside 1..9 blink as 9. */
static int bs_blink_level(unsigned pulses, unsigned tick)
{
    unsigned round_len, pos;
    if (pulses < 1 || pulses > 9) pulses = 9;
    round_len = BS_BLINK_GAP_TICKS + pulses * (BS_BLINK_ON_TICKS + BS_BLINK_OFF_TICKS);
    if (tick >= round_len * BS_BLINK_ROUNDS) return -1;
    pos = tick % round_len;
    if (pos < BS_BLINK_GAP_TICKS) return 0;
    pos = (pos - BS_BLINK_GAP_TICKS) % (BS_BLINK_ON_TICKS + BS_BLINK_OFF_TICKS);
    return pos < BS_BLINK_ON_TICKS ? (int)BS_BLINK_LEVEL : 0;
}

static uint16_t bs_cap16(uint32_t v)
{
    return v > 65535u ? 65535u : (uint16_t)v;
}

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
        /* Judge on the newest sample at least a minute old; with a full history
         * also on the oldest one, even if timer_get_ms() makes it a little short. */
        if (span < BS_WD_STALL_MS && !(k == w->count - 1u && w->count == BS_WD_SAMPLES))
            continue;
        progressed = loops - w->loops[i];
        if (progressed == 0
            || (uint64_t)progressed * 60000u < (uint64_t)BS_WD_MIN_LOOPS_PER_MIN * span)
            w->stalled_ms = span ? span : 1u;
        return w->stalled_ms != 0;
    }
    return 0;   /* less than a full history yet */
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
static int bs_page(const bs_sleep *s, const bs_watchdog *w, const bs_diag *d, uint32_t te,
                   uint32_t loops, uint32_t now_ms, const uint8_t *request, unsigned length,
                   uint8_t *reply)
{
    unsigned page;
    uint32_t stalled_s;
    if (length != 4 || request[0] != 'B' || request[1] != 'S' || request[2] != 1
        || request[3] > 3) return 0;
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
    } else if (page == 3) {
        bs_u32(reply + 4, d->calls);
        reply[8] = d->stage;
        reply[9] = d->blinking ? 1 : 0;
        reply[10] = (uint8_t)d->interval_ms; reply[11] = (uint8_t)(d->interval_ms >> 8);
        reply[12] = (uint8_t)d->interval_jiffies_ms;
        reply[13] = (uint8_t)(d->interval_jiffies_ms >> 8);
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
static bs_diag bs_d;
static volatile uint8_t bs_stage = BS_STAGE_BOOT;
static volatile uint8_t bs_blink_pulses;   /* 0 = not blinking */
static volatile uint16_t bs_blink_tick;
static uint8_t bs_blink_wd_calls;
static uint32_t bs_last_ms, bs_last_jiffies_ms;

/* UI task: record the current step of the sleep/wake path (BS_STAGE_*). */
static inline void badge_stage(uint8_t stage)
{
    bs_stage = stage;
}

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

extern unsigned long jiffies_msec();
/* SDK lcd_drive.c. On the E87 (ST77916, .backlight_ctrl NULL, TCFG_BACKLIGHT_PWM_MODE 2,
 * pin IO_LCD_PG) this is power_gate_pwm_set_duty(): a register write under
 * local_irq_disable/enable, no printf for 0..100, no semaphore, no P33 access. */
extern int lcd_drv_backlight_ctrl_base(u8 percent);

/* Hardware-timer context (same timer class as the ADKEY scan), so it still runs
 * when tasks are starved. */
static void bs_watchdog_timer(void *priv)
{
    uint32_t now = timer_get_ms(), jiffies_ms = (uint32_t)jiffies_msec();
    uint8_t stage;
    (void)priv;
    if (++bs_d.calls > 1) {
        bs_d.interval_ms = bs_cap16(now - bs_last_ms);
        bs_d.interval_jiffies_ms = bs_cap16(jiffies_ms - bs_last_jiffies_ms);
    }
    bs_last_ms = now;
    bs_last_jiffies_ms = jiffies_ms;
    if (bs_blink_pulses) {
        /* Signalling; the blink timer resets at the end. If it never runs, reset here. */
        if (++bs_blink_wd_calls >= BS_BLINK_MAX_WD_CALLS) bs_reset();
        return;
    }
    if (bs_watchdog_check(&bs_wd, now, bs_ui_loops,
                          g_badge_update_active || g_on_update_scr)) {
#if BADGE_SAFETY_WATCHDOG
        stage = bs_stage;
        bs_blink_tick = 0;
        bs_blink_wd_calls = 0;
        bs_blink_pulses = (stage >= 1 && stage <= 9) ? stage : 9;   /* hands over to bs_blink_timer */
#endif
    }
}

/* Hardware-timer context, every BS_BLINK_TICK_MS. Idle unless the watchdog fired. */
static void bs_blink_timer(void *priv)
{
    int level;
    uint8_t pulses = bs_blink_pulses;
    (void)priv;
    if (!pulses) return;
    level = bs_blink_level(pulses, bs_blink_tick);
    bs_blink_tick++;
    if (level < 0) bs_reset();
    lcd_drv_backlight_ctrl_base((u8)level);
}

static void badge_safety_start(void)
{
    static uint8_t started;
    if (started) return;
    started = 1;
    sys_hi_timer_add(NULL, bs_watchdog_timer, BS_WD_PERIOD_MS);
    sys_hi_timer_add(NULL, bs_blink_timer, BS_BLINK_TICK_MS);
}

/* BLE task: read-only snapshot of the counters. */
void badge_safety_reply(const u8 *data, u16 len);
int badge_safety_command(u8 cmd, const u8 *p, u16 n)
{
    uint8_t reply[BS_PAGE_SIZE];
    int length;
    if (cmd != BS_CMD) return 0;
    bs_d.stage = bs_stage;
    bs_d.blinking = bs_blink_pulses != 0;
    length = bs_page(&bs_sleep_state, &bs_wd, &bs_d, bs_te_irqs, bs_ui_loops, timer_get_ms(),
                     p, n, reply);
    if (length) badge_safety_reply(reply, (u16)length);
    return 1;
}
#endif
#endif
