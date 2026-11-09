/* Task waits, not hardware sleep. OS semaphore posts are safe from the key timer.
 * A post between checking a condition and pend() is retained: no lost wake-up.
 * The SDK uses 10 ms OS ticks. A failed allocation keeps a bounded polling path. */
#ifndef BADGE_IDLE_EVENT_H
#define BADGE_IDLE_EVENT_H
#include <stdint.h>
typedef struct {
    OS_SEM sem;
    volatile uint8_t ready;
} badge_idle_event;
static void badge_idle_event_init(badge_idle_event *event)
{
    event->ready = (os_sem_create(&event->sem, 0) == 0);
}
static void badge_idle_event_signal(badge_idle_event *event)
{
    if (event->ready) os_sem_post(&event->sem);
}
static void badge_idle_event_wait(badge_idle_event *event, unsigned ticks)
{
    if (event->ready) os_sem_pend(&event->sem, ticks);
    else os_time_dly(ticks ? ticks : 10);
}
#endif
