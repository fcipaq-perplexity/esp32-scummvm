/*
 * Settings of the on-board menu, stored on the SD card in /sdcard/settings.dat as text lines
 * "key=value" (brightness, volume, headphones). Missing or invalid values use the defaults.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TABLET_SETTINGS_FILE        "/sdcard/settings.dat"
#define TABLET_SETTINGS_BRIGHT_MIN  5     /* the menu must stay readable */

typedef struct {
    int brightness;     /* TABLET_SETTINGS_BRIGHT_MIN..100 */
    int volume;         /* 0..100 */
    bool headphones;
} tablet_settings_t;

/* Reads the file (the SD card must be mounted). Defaults if it does not exist. */
void tablet_settings_load(void);
/* Applies brightness, volume and audio output to the hardware */
void tablet_settings_apply(void);
/* Current values (owned by this module) */
tablet_settings_t *tablet_settings(void);
/* Writes the file if something changed since the last load or save */
void tablet_settings_save(void);

#ifdef __cplusplus
}
#endif
