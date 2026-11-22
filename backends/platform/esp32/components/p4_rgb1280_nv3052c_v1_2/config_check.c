/*
 * Compile-time check of the start-up configuration of board p4_rgb1280_nv3052c_v1_2 (ScummVM port).
 *
 * The bounce buffers of display_lcd (board_devices.yaml) feed the panel; the RGB driver in
 * components/esp_lcd needs its ISR to stay in IRAM while the flash cache is disabled.
 */
#include "sdkconfig.h"

#if !CONFIG_LCD_RGB_ISR_IRAM_SAFE
#error "Board p4_rgb1280_nv3052c_v1_2 needs CONFIG_LCD_RGB_ISR_IRAM_SAFE=y. Stale sdkconfig? Delete it and run idf.py bmgr -b p4_rgb1280_nv3052c_v1_2 again"
#endif

typedef int board_config_check_t;
