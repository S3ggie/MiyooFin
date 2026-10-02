/*
 * MENU is also half of OnionOS's brightness combo (MENU + volume up/down, handled
 * by keymon). Exit on MENU only when it was a plain tap: released with no other key
 * pressed meanwhile and the backlight unchanged. Plain C99 so both the C player and
 * the C++ app (and the host tests) share it.
 */
#ifndef MIYOOFIN_MENU_GUARD_H
#define MIYOOFIN_MENU_GUARD_H

#include <stdio.h>

#define MENU_GUARD_BACKLIGHT "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"

typedef struct MenuGuard {
    int held, other_key;
    long brightness_at_down; /* -1 = unreadable */
} MenuGuard;

/* Current backlight duty cycle, or -1 when it can't be read. */
static inline long menu_guard_read_backlight(const char *path)
{
    long v = -1;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static inline void menu_guard_down(MenuGuard *g, long brightness_now)
{
    if (g->held)
        return; /* key repeat */
    g->held = 1;
    g->other_key = 0;
    g->brightness_at_down = brightness_now;
}

/* Any key other than MENU going down while MENU is held makes it a combo. */
static inline void menu_guard_other_key(MenuGuard *g)
{
    if (g->held)
        g->other_key = 1;
}

/* MENU released: 1 when this was a plain tap that should exit. */
static inline int menu_guard_up(MenuGuard *g, long brightness_now)
{
    int tap = g->held && !g->other_key &&
              (g->brightness_at_down < 0 || brightness_now == g->brightness_at_down);
    g->held = 0;
    g->other_key = 0;
    return tap;
}

#endif
