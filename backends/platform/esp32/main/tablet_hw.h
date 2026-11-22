/*
 * Board v1.2 extras that are not Board Manager devices: power hold and power off, battery
 * measurement and the menu button. On boards without them (Kconfig) the functions do nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in app_main: float the power off pin, drive the power hold pin high */
void tablet_power_hold(void);

/* Switches the board off. Does not return if the board has a power off pin. */
void tablet_power_off(void);

/* True if the board can switch itself off */
bool tablet_can_power_off(void);

typedef struct {
    uint16_t millivolts;
    uint8_t percent;
    bool valid, charging, calibrated;
} tablet_battery_t;

esp_err_t tablet_battery_init(void);
void tablet_battery_get(tablet_battery_t *status);

/* Polls the menu button, a press calls tablet_ui_toggle() */
esp_err_t tablet_button_init(void);

#ifdef __cplusplus
}
#endif
