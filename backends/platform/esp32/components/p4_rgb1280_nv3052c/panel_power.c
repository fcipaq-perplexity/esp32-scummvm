/*
 * Panel supply of the 720x1280 NV3052C board: switches on an LDO channel of the ESP32-P4.
 *
 * Registered as the Board Manager custom device "panel_power" (see board_devices.yaml). The
 * application brings this device up before Wi-Fi starts, if the board defines it; lcd_spi_init
 * depends on it. Channel, voltage and settle time come from the board definition, so other
 * boards do not have to touch this rail at all.
 */
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dev_custom.h"
#include "gen_board_device_custom.h"

static const char *TAG = "panel_power";

static esp_ldo_channel_handle_t s_ldo;

static int panel_power_dev_init(void *config, int cfg_size, void **device_handle)
{
    dev_custom_panel_power_config_t *cfg = (dev_custom_panel_power_config_t *) config;
    if (cfg_size != (int) sizeof(*cfg)) {
        ESP_LOGE(TAG, "Invalid config size: %d, expected: %d", cfg_size, (int) sizeof(*cfg));
        return -1;
    }

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = cfg->ldo_chan_id,
        .voltage_mv = cfg->ldo_voltage_mv,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire LDO channel %d: %s", cfg->ldo_chan_id, esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "LDO channel %d enabled at %d mV", cfg->ldo_chan_id, cfg->ldo_voltage_mv);

    if (cfg->settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->settle_ms));
    }

    *device_handle = &s_ldo;
    return 0;
}

static int panel_power_dev_deinit(void *device_handle)
{
    (void) device_handle;
    if (s_ldo) {
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
    }
    return 0;
}

CUSTOM_DEVICE_IMPLEMENT(panel_power, panel_power_dev_init, panel_power_dev_deinit);
