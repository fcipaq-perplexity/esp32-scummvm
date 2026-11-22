/*
 * Display of the tablet boards: 720x1280 RGB panel (Board Manager device display_lcd) used in
 * landscape. Everything above this layer works in landscape coordinates (TABLET_W x TABLET_H);
 * the PPA scales and rotates pictures into the two framebuffers of the RGB panel.
 *
 * Owned by one lock: game frames, the on-board menu and overlay scaling never touch the
 * framebuffers or the PPA at the same time. While the menu is open, game frames are dropped.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TABLET_W 1280
#define TABLET_H 720

/* Panel power, panel init (on core 1, so its interrupts stay there), PPA. Backlight off. */
esp_err_t tablet_lcd_init(void);

/* Shows an RGB565 picture (packed rows, w x h) scaled to fit the screen, aspect ratio kept,
 * centered with black bars. Waits for the previous flip. ESP_ERR_INVALID_STATE while the menu is open. */
esp_err_t tablet_lcd_present(const uint16_t *src, int w, int h);

/* Where a w x h picture ends up on the landscape screen (same rule as tablet_lcd_present) */
void tablet_lcd_viewport(int w, int h, int *ox, int *oy, int *ow, int *oh);

/* Scales a w x h RGB565 picture like tablet_lcd_present into a TABLET_W x TABLET_H RGB565
 * buffer (not rotated; dst must be 128-byte aligned, borders are filled black). */
esp_err_t tablet_lcd_scale_to_screen(const uint16_t *src, int w, int h, uint16_t *dst);

/* On-board menu session: begin, present full TABLET_W x TABLET_H RGB565 pictures, end (the game
 * picture comes back). */
esp_err_t tablet_lcd_ui_begin(void);
esp_err_t tablet_lcd_ui_present(const uint16_t *src);
void tablet_lcd_ui_end(void);

/* Backlight 0..100 % (Board Manager device lcd_brightness) */
esp_err_t tablet_backlight_set(int percent);

/* Touch controller (Board Manager device lcd_touch). Init starts a polling task on core 0.
 * ESP_ERR_NOT_FOUND if the board has none. */
esp_err_t tablet_touch_init(void);

/* Latest contacts for ScummVM in landscape coordinates (none while the menu owns the touch).
 * Returns the number of contacts, fills at most max points. */
int tablet_touch_get(uint16_t *x, uint16_t *y, int max);

#ifdef __cplusplus
}
#endif
