// SPDX-License-Identifier: MIT
// USB (and any evdev) keyboards as one more input of the REPL hub: see keyboard.c.
#ifndef MPY_REPL_HUB_KEYBOARD_H
#define MPY_REPL_HUB_KEYBOARD_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#define KEYBOARD_MAX 4

// Chooses the layout: "us", "it", or "none" for no keyboards. False for an unknown name.
bool keyboard_layout(const char *name);
// The settings.json whose "keyboard" section chooses the layout, instead of keyboard_layout's.
void keyboard_settings(const char *path);
// Lets the keyboards go while another program borrows them (/run/mpy-keyboard-lent), and notes
// when they come back; cheap enough to call at every turn of the loop.
void keyboard_lend(void);
// Opens the keyboards plugged in since the last call; cheap enough to call every second.
void keyboard_scan(void);
// Adds the open keyboards to fds; returns how many.
size_t keyboard_poll_fds(struct pollfd *fds, short events);
// Reads the keyboards whose slot in fds reports input, and appends the bytes their keys type
// (UTF-8, VT100 sequences for the cursor and function keys) to out. Returns the new length.
size_t keyboard_read(const struct pollfd *fds, size_t count, unsigned char *out, size_t length, size_t size);

#endif
