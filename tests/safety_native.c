/* Native checks for badge_safety.h: watchdog policy, sleep tracking, diagnostics. */
#include <assert.h>
#include <stdio.h>
#include "badge_safety.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Feed one check per period; returns 1 if any check asked for a reset. */
static int run(bs_watchdog *w, uint32_t *t, uint32_t *loops, int periods, uint32_t per_period,
               int exempt)
{
    int i, reset = 0;
    for (i = 0; i < periods; i++) {
        *t += BS_WD_PERIOD_MS; *loops += per_period;
        reset |= bs_watchdog_check(w, *t, *loops, exempt);
    }
    return reset;
}

static void test_watchdog(void)
{
    bs_watchdog w = {0};
    uint32_t t = 1000, loops = 0;
    int i, reset;
    assert(!bs_watchdog_check(&w, t, loops, 0));          /* first sample: span 0 */
    assert(!run(&w, &t, &loops, 120, 500, 0));             /* ~100 loops/s for 10 min */
    assert(!run(&w, &t, &loops, 120, 5, 0));               /* fix/lock-power wait, ~1/s */
    assert(!run(&w, &t, &loops, 120, 165, 0));             /* old 30 ms lock poll, ~33/s */

    /* Codex counterexample: 3 passes once every 50 s (0.06/s) must reset. */
    w = (bs_watchdog){0}; reset = 0;
    for (i = 0; i < 40 && !reset; i++) {
        t += BS_WD_PERIOD_MS;
        if (i % 10 == 0) loops += 3;
        reset = bs_watchdog_check(&w, t, loops, 0);
    }
    assert(reset && i <= 13);

    /* A full stall resets once the window spans 60 s, not before. */
    w = (bs_watchdog){0};
    assert(!bs_watchdog_check(&w, t, loops, 0));
    assert(!run(&w, &t, &loops, 11, 0, 0));
    assert(run(&w, &t, &loops, 1, 0, 0));

    /* Below the floor (0.4/s) resets; above it (0.6/s) does not. */
    w = (bs_watchdog){0};
    assert(run(&w, &t, &loops, 13, 2, 0));
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 60, 3, 0));

    /* fix/lock-power lock (~1/s), then a button held 25 s, then awake: no reset. */
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 12, 5, 0));
    assert(!run(&w, &t, &loops, 5, 0, 0));
    assert(!run(&w, &t, &loops, 12, 500, 0));
    /* Held 35 s from the lock screen: reset (documented, about 30 s). */
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 12, 5, 0));
    assert(run(&w, &t, &loops, 7, 0, 0));
    /* Held 55 s while awake: no reset; a full minute: reset (documented). */
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 12, 500, 0));
    assert(!run(&w, &t, &loops, 11, 0, 0));
    assert(!run(&w, &t, &loops, 12, 500, 0));
    w = (bs_watchdog){0};
    assert(!bs_watchdog_check(&w, t, loops, 0));
    assert(run(&w, &t, &loops, 12, 0, 0));

    /* A timer that itself ran 70 s late sees the stall at once. */
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 3, 500, 0));
    t += 70000; loops += 3;
    assert(bs_watchdog_check(&w, t, loops, 0));

    /* Update screen / active update: never resets, and re-arms afterwards. */
    w = (bs_watchdog){0};
    assert(!run(&w, &t, &loops, 100, 0, 1));
    assert(w.exempt == 1 && w.stalled_ms == 0 && w.count == 0);
    assert(!run(&w, &t, &loops, 12, 0, 0) && w.exempt == 0);   /* fresh window: needs 60 s of history */
    assert(run(&w, &t, &loops, 1, 0, 0));

    /* Millisecond and loop counter wrap. */
    w = (bs_watchdog){0};
    t = 0xFFFFF000u; loops = 0xFFFFFF00u;
    assert(!run(&w, &t, &loops, 30, 500, 0) && w.stalled_ms == 0);
}

static void test_sleep_and_pages(void)
{
    bs_sleep s = {0};
    bs_watchdog w = {0};
    uint8_t reply[BS_PAGE_SIZE];
    const uint8_t p0[4] = {'B', 'S', 1, 0}, p1[4] = {'B', 'S', 1, 1}, p2[4] = {'B', 'S', 1, 2};
    const uint8_t bad_magic[4] = {'B', 'P', 1, 0}, bad_page[4] = {'B', 'S', 1, 3};

    assert(bs_page(&s, &w, 7, 9, 0, bad_magic, 4, reply) == 0);
    assert(bs_page(&s, &w, 7, 9, 0, bad_page, 4, reply) == 0);
    assert(bs_page(&s, &w, 7, 9, 0, p0, 3, reply) == 0);
    /* No sleep yet: pages 1 and 2 are zero. */
    assert(bs_page(&s, &w, 7, 9, 0, p1, 4, reply) == BS_PAGE_SIZE && rd32(reply + 4) == 0);
    assert(bs_page(&s, &w, 7, 9, 0, p2, 4, reply) == BS_PAGE_SIZE && rd32(reply + 8) == 0);

    bs_sleep_exit(&s, 10, 100);                 /* exit without enter: ignored */
    assert(s.flags == 0);
    bs_sleep_enter(&s, 1000, 50);
    bs_sleep_enter(&s, 2000, 60);               /* repeated enter keeps the first */
    assert(bs_page(&s, &w, 4050, 9, 61000, p2, 4, reply) == BS_PAGE_SIZE);
    assert(rd32(reply + 4) == 4000 && rd32(reply + 8) == 60);
    assert(bs_page(&s, &w, 4050, 9, 61000, p0, 4, reply) == BS_PAGE_SIZE);
    assert(reply[12] == BS_ASLEEP && rd32(reply + 4) == 4050);
    bs_sleep_exit(&s, 3601000, 5050);
    assert(bs_page(&s, &w, 5050, 9, 3601000, p1, 4, reply) == BS_PAGE_SIZE);
    assert(memcmp(reply, p1, 4) == 0 && rd32(reply + 4) == 5000 && rd32(reply + 8) == 3600);
    assert(bs_page(&s, &w, 5050, 9, 3601000, p2, 4, reply) == BS_PAGE_SIZE && rd32(reply + 4) == 0);

    w.stalled_ms = 1000000; w.exempt = 1;
    assert(bs_page(&s, &w, 5050, 123456, 0, p0, 4, reply) == BS_PAGE_SIZE);
    assert(rd32(reply + 8) == 123456 && reply[12] == (BS_LAST_VALID | BS_EXEMPT) && reply[13] == 255);
}

int main(void)
{
    test_watchdog();
    test_sleep_and_pages();
    puts("safety: ok");
    return 0;
}
