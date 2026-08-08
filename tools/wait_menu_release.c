/*
 * wait_menu_release.c — Wait for the physical MENU button release event.
 *
 * After FFplay exits on a MENU press, this helper keeps /tmp/disable_menu_button
 * in place while waiting for the user to release the button.  Onion keymon
 * cannot fire its global MENU shortcut while the flag exists, and this helper
 * reads /dev/input/event0 (Linux evdev allows concurrent readers) looking for
 * the corresponding EV_KEY release.
 *
 * Returns 0 if release detected, 1 on timeout or error.
 * Called from launch.sh with a bounded timeout of 1.5 seconds.
 */

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>

/* Linux input_event with fixed-size fields — 16 bytes on all Linux arches. */
struct evdev_event {
    uint64_t time;   /* struct timeval (8 bytes on 32-bit LE) */
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_KEY      0x01
#define KEY_UP      0    /* evdev: value 0 = key release */
#define KEY_ESC     1    /* Linux keycode for SDL_SCANCODE_ESCAPE (41) */
#define KEY_MENU    139  /* Linux keycode for KEY_MENU (fallback) */
#define DEVICE      "/dev/input/event0"
#define TIMEOUT_MS  1500 /* 1.5 seconds — generous bound for button release */
#define POLL_STEP   50   /* poll in 50 ms increments */

int main(void)
{
    int fd = open(DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 1;

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;

    int remaining = TIMEOUT_MS;

    while (remaining > 0) {
        int ms = (remaining < POLL_STEP) ? remaining : POLL_STEP;
        int ret = poll(&pfd, 1, ms);
        remaining -= ms;

        if (ret <= 0)
            continue;

        struct evdev_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
            continue;

        /* Detect release (value == 0) of the MENU / ESC key. */
        if (ev.type == EV_KEY && ev.value == KEY_UP &&
            (ev.code == KEY_ESC || ev.code == KEY_MENU)) {
            close(fd);
            return 0;
        }
    }

    close(fd);
    return 1; /* timeout or error */
}
