/* No hardware writes: debounce charger presence and format a bounded status. */
#ifndef BADGE_POWER_STATUS_H
#define BADGE_POWER_STATUS_H
#include <stdint.h>
/* The embedding translation unit provides snprintf. Do not include the
 * toolchain stdio.h here: the vendor SDK defines its own filesystem FILE. */
typedef struct {
    uint8_t initialized, online, candidate, count;
} badge_power_state;
static void badge_power_sample(badge_power_state *s, int present)
{
    uint8_t value = !!present;
    if (!s->initialized) {
        s->initialized = 1; s->online = s->candidate = value; s->count = 0;
    } else if (value != s->candidate) {
        s->candidate = value; s->count = 1;
    } else if (s->online != value && ++s->count >= 3) {
        s->online = value; s->count = 0;
    }
}
static unsigned badge_power_text(char *text, unsigned capacity, unsigned percent,
                                int online, int full)
{
    unsigned color = online ? 0x40ff80 : (percent <= 20 ? 0xffb000 : 0xffffff);
    if (percent > 100) percent = 100;
    snprintf(text, capacity, "%u%%  %s", percent,
             online ? (full ? "FULL" : "CHARGING") : "BATTERY");
    return color;
}
#endif
