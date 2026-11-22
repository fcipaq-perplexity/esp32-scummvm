/*
 * Board specific factory functions of p4_rgb1280_nv3052c_v1_2.
 *
 * lcd_touch: the Board Manager calls lcd_touch_factory_entry_t() when it initialises the lcd_touch
 * device (GT911, see board_devices.yaml). Weak symbol and __has_include as in the Espressif boards,
 * so that an amend can replace it.
 */
#include "esp_log.h"
#include "esp_board_manager_includes.h"
#if __has_include(<esp_lcd_touch_gt911.h>)
#define HAS_GT911  1
#include "esp_lcd_touch_gt911.h"
#endif  /* __has_include(<esp_lcd_touch_gt911.h>) */

#if defined(HAS_GT911)
__attribute__((weak)) esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    /*
     * With the GT911 specific config the driver runs the full reset: RST low, INT low (= I2C address
     * 0x5D), RST high, then waits 60 ms until the chip has booted. Without it (driver_data NULL) the
     * driver only pulses RST and reads the chip 10 ms later, which is too early: the read fails.
     * The address must be the one the Board Manager probed (0x5D, 8-bit 0xBA, first candidate of the
     * lcd_touch device; panel_power selects it with INT low, too). The driver only uses the pointer
     * while it is created, static anyway.
     */
    static esp_lcd_touch_io_gt911_config_t gt911_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    esp_lcd_touch_config_t cfg = *touch_dev_config;
    cfg.driver_data = &gt911_io_cfg;

    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, &cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_touch_factory_entry_t", "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
#endif  /* defined(HAS_GT911) */
