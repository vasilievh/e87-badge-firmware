/* RAM-only power diagnostics. Percent and mV are cached SDK estimates, not a
 * current measurement. Four 14-byte pages fit a 20-byte Qix frame / ATT MTU 23. */
#ifndef BADGE_IDLE_STATUS_H
#define BADGE_IDLE_STATUS_H
#include <stdint.h>
#include <string.h>
#define BI_CMD 0x92
#define BI_REPLY 0x93
#define BI_PAGE_SIZE 14
#define BI_LOCKED 1
#define BI_USB 2
#define BI_FULL 4
#define BI_BLE_WANTED 8
#define BI_LAST_VALID 16
typedef struct {
    uint32_t now_ms, entered_ms, last_ms, waits;
    uint32_t sys_hz, idle_hz;
    uint16_t mv, entered_mv, exited_mv, last_entered_mv;
    uint8_t percent, entered_percent, exited_percent, last_entered_percent, flags, lowpower;
    int8_t unlock_result, lock_result;
} badge_idle_status;
static void bi_enter(badge_idle_status *s)
{
    if (s->flags & BI_LOCKED) return;
    s->entered_ms = s->now_ms;
    s->entered_mv = s->mv; s->entered_percent = s->percent;
    s->flags |= BI_LOCKED;
}
static void bi_exit(badge_idle_status *s)
{
    if (!(s->flags & BI_LOCKED)) return;
    s->last_ms = s->now_ms - s->entered_ms;
    s->last_entered_mv = s->entered_mv; s->last_entered_percent = s->entered_percent;
    s->exited_mv = s->mv; s->exited_percent = s->percent;
    s->flags = (s->flags & ~BI_LOCKED) | BI_LAST_VALID;
}
static void bi_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void bi_u32(uint8_t *p, uint32_t v)
{
    bi_u16(p, (uint16_t)v); bi_u16(p + 2, (uint16_t)(v >> 16));
}
static int bi_page(const badge_idle_status *s, const uint8_t *request,
                   unsigned length, uint8_t *reply)
{
    unsigned page;
    if (length != 4 || request[0] != 'B' || request[1] != 'P' || request[2] != 1
        || request[3] > 3) return 0;
    page = request[3];
    memset(reply, 0, BI_PAGE_SIZE);
    memcpy(reply, request, 4);
    if (page == 0) {
        reply[4] = s->flags; reply[5] = s->percent;
        bi_u16(reply + 6, s->mv); bi_u32(reply + 8, s->now_ms / 1000);
        reply[12] = s->lowpower;
    } else if (page == 1) {
        reply[4] = s->last_entered_percent; reply[5] = s->exited_percent;
        bi_u16(reply + 6, s->last_entered_mv); bi_u16(reply + 8, s->exited_mv);
        bi_u32(reply + 10, s->last_ms / 1000);
    } else if (page == 2) {
        bi_u32(reply + 4, (s->flags & BI_LOCKED) ? (s->now_ms - s->entered_ms) / 1000 : 0);
        bi_u32(reply + 8, s->waits);
    } else {
        bi_u32(reply + 4, s->sys_hz); bi_u32(reply + 8, s->idle_hz);
        reply[12] = (uint8_t)s->unlock_result; reply[13] = (uint8_t)s->lock_result;
    }
    return BI_PAGE_SIZE;
}
#endif
