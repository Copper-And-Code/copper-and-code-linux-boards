// SPDX-License-Identifier: MIT
// time.sleep_ms() (mp_hal_delay_ms) for this board's MicroPython: the Unix port's own sleeps in
// steps of 0.5 ms and takes the GIL back after each one, 20 times for 10 ms. With threads on a
// single core (STM32MP135) each of those waits for the thread that holds the GIL, and they add up:
// a thread's sleep_ms(10) took 33 ms on average and up to 640 ms while another thread ran.
//
// Here the sleep goes in steps of up to 10 ms, the GIL released for each: pending events (Ctrl-C,
// scheduled callbacks) are handled between steps, and a signal ends a step at once (EINTR).

#include <time.h>
#include <unistd.h>

#include "py/mphal.h"
#include "py/runtime.h"

#define STEP_US 10000

static uint64_t now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

void mpy_linux_delay_ms(uintptr_t ms) {
    if (ms == 0) {
        mp_handle_pending(true);
        return;
    }
    uint64_t end = now_us() + (uint64_t)ms * 1000u;
    for (;;) {
        mp_event_handle_nowait();
        uint64_t now = now_us();
        if (now >= end) {
            break;
        }
        uint64_t left = end - now;
        MP_THREAD_GIL_EXIT();
        usleep(left > STEP_US ? STEP_US : (useconds_t)left);
        MP_THREAD_GIL_ENTER();
    }
}
