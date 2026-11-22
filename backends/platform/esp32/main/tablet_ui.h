/*
 * On-board menu (brightness, volume, audio output, turn off), opened and closed with the menu
 * button. Drawn in landscape into an offscreen buffer and shown by tablet_lcd. While it is open
 * ScummVM is paused (main.cpp) and gets no touch input. See ui_menu.h for the logic.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tablet_ui_init(void);
void tablet_ui_toggle(void);
bool tablet_ui_active(void);

/* Called by the touch task on every poll with landscape coordinates.
 * True if the menu takes the contacts (also until the finger is lifted after it closed). */
bool tablet_ui_touch(int cnt, const uint16_t *x, const uint16_t *y);

#ifdef __cplusplus
}
#endif
