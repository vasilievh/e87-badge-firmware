/* Native checks for badge_safety.h: watchdog policy, sleep tracking, diagnostics. */
#include <assert.h>
#include <stdio.h>
#include "badge_safety.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_watchdog(void)
{
    bs_watchdog w = {0};
    uint32_t t = 1000, loops = 0;
    int i;
    /* First check only arms. */
    assert(!bs_watchdog_check(&w, t, loops, 0));
    /* Normal UI (~100 loops/s) for 10 minutes: never resets. */
    for (i = 0; i < 120; i++) {
        t += BS_WD_PERIOD_MS; loops += 500;
        assert(!bs_watchdog_check(&w, t, loops, 0));
    }
    /* fix/lock-power lock wait: ~1 loop/s stays well above 0.5/s. */
    for (i = 0; i < 120; i++) {
        t += BS_WD_PERIOD_MS; loops += 5;
        assert(!bs_watchdog_check(&w, t, loops, 0));
    }
    /* Starved UI (one pass per 20 s) resets after 60 s of stall, not before. */
    for (i = 0; i < 11; i++) {
        t += BS_WD_PERIOD_MS; loops += (i % 4 == 0);
        assert(!bs_watchdog_check(&w, t, loops, 0));
    }
    t += BS_WD_PERIOD_MS;
    assert(bs_watchdog_check(&w, t, loops, 0));
    /* Recovery clears the stall. */
    w = (bs_watchdog){0};
    assert(!bs_watchdog_check(&w, t, loops, 0));
    t += BS_WD_PERIOD_MS * 6; /* 30 s of total stall */
    assert(!bs_watchdog_check(&w, t, loops, 0));
    t += BS_WD_PERIOD_MS; loops += 500;
    assert(!bs_watchdog_check(&w, t, loops, 0) && w.stalled_ms == 0);
    /* A timer that itself ran 70 s late sees the stall at once. */
    t += 70000; loops += 3;
    assert(bs_watchdog_check(&w, t, loops, 0));
    /* Update screen / active update: never resets, and re-arms afterwards. */
    w = (bs_watchdog){0};
    for (i = 0; i < 100; i++) {
        t += BS_WD_PERIOD_MS;
        assert(!bs_watchdog_check(&w, t, loops, 1));
        assert(w.exempt == 1 && w.stalled_ms == 0);
    }
    t += BS_WD_PERIOD_MS;
    assert(!bs_watchdog_check(&w, t, loops, 0) && w.exempt == 0);
    /* Millisecond counter wrap. */
    w = (bs_watchdog){0};
    t = 0xFFFFF000u; loops = 0xFFFFFF00u;
    assert(!bs_watchdog_check(&w, t, loops, 0));
    t += BS_WD_PERIOD_MS; loops += 500;
    assert(!bs_watchdog_check(&w, t, loops, 0) && w.stalled_ms == 0);
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
