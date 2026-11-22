/*
 * Power preparation of the 720x1280 NV3052C board v1.2.
 *
 * Registered as the Board Manager custom device "panel_power" (see board_devices.yaml). The
 * application brings this device up before Wi-Fi and the LCD, if the board defines it;
 * lcd_spi_init depends on it. It does, in this order:
 *  1. optional LDO channel (ldo_chan_id >= 0)
 *  2. optional power enable GPIO (pwr_en_gpio >= 0). On this board it is the enable pin of the main
 *     voltage regulator: the level keeps the whole board powered. The firmware never drives it to
 *     the inactive level, that would switch the board off.
 *  3. optional parking of the panel's 3-wire SPI lines (spi_sck_gpio, spi_mosi_gpio >= 0): both
 *     are driven low as outputs. The panel's chip select is always asserted, so noise on these
 *     lines between its reset and the init would shift the alignment of the 9-bit words.
 *  4. settle time
 *  5. optional reset of the GT911 touch controller (touch_rst_gpio >= 0), see touch_reset()
 *  6. optional reset pulse (rst_gpio >= 0): low for rst_low_ms, high for rst_wait_ms. The pin stays
 *     an output driven high afterwards. The reset line is shared by the enable pin of the C6
 *     co-processor and the reset pin of the panel, so this resets both. Not used on board v1.2:
 *     lcd_spi_init gives the reset pulse right before the panel init (rst_gpio is -1 there).
 */
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dev_custom.h"
#include "gen_board_device_custom.h"

static const char *TAG = "panel_power";

static esp_ldo_channel_handle_t s_ldo;

static void park_pin_low(int gpio)
{
    if (gpio < 0) {
        return;
    }
    gpio_reset_pin((gpio_num_t) gpio);
    gpio_set_level((gpio_num_t) gpio, 0);
    gpio_set_direction((gpio_num_t) gpio, GPIO_MODE_OUTPUT);
}

/*
 * Reset of the GT911 with the I2C address select: INT low while RST rises selects the address
 * 0xBA/0xBB (7-bit 0x5D). The Board Manager probes the address of the touch controller before the
 * driver gets to reset it, and a GT911 that has not been reset since the power-up does not answer
 * there. This is the same sequence as the driver uses; INT is released afterwards (the GT911 drives
 * it), RST stays an output driven high.
 */
static void touch_reset(int rst_gpio, int int_gpio)
{
    if (rst_gpio < 0) {
        return;
    }
    if (int_gpio >= 0) {
        gpio_reset_pin((gpio_num_t) int_gpio);
        gpio_set_level((gpio_num_t) int_gpio, 0);
        gpio_set_direction((gpio_num_t) int_gpio, GPIO_MODE_OUTPUT);
    }
    gpio_reset_pin((gpio_num_t) rst_gpio);
    gpio_set_level((gpio_num_t) rst_gpio, 0);
    gpio_set_direction((gpio_num_t) rst_gpio, GPIO_MODE_OUTPUT);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t) rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (int_gpio >= 0) {
        gpio_set_direction((gpio_num_t) int_gpio, GPIO_MODE_INPUT);
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    ESP_LOGI(TAG, "Touch controller reset (RST GPIO %d, INT GPIO %d, address 0x5D)", rst_gpio, int_gpio);
}

static int panel_power_dev_init(void *config, int cfg_size, void **device_handle)
{
    dev_custom_panel_power_config_t *cfg = (dev_custom_panel_power_config_t *) config;
    if (cfg_size != (int) sizeof(*cfg)) {
        ESP_LOGE(TAG, "Invalid config size: %d, expected: %d", cfg_size, (int) sizeof(*cfg));
        return -1;
    }

    if (cfg->ldo_chan_id >= 0) {
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
    }
#if 0
    // pwr_en needs to be floating to shut the system down when long press the button
    if (cfg->pwr_en_gpio >= 0) {
        int level = cfg->pwr_en_level ? 1 : 0;
        gpio_reset_pin((gpio_num_t) cfg->pwr_en_gpio);
        gpio_set_level((gpio_num_t) cfg->pwr_en_gpio, level);
        gpio_set_direction((gpio_num_t) cfg->pwr_en_gpio, GPIO_MODE_OUTPUT);
        ESP_LOGI(TAG, "Power enable GPIO %d set to %d", cfg->pwr_en_gpio, level);
    }
#endif
    park_pin_low(cfg->spi_sck_gpio);
    park_pin_low(cfg->spi_mosi_gpio);
    if (cfg->spi_sck_gpio >= 0 || cfg->spi_mosi_gpio >= 0) {
        ESP_LOGI(TAG, "Panel SPI lines parked low (SCK GPIO %d, MOSI GPIO %d)", cfg->spi_sck_gpio, cfg->spi_mosi_gpio);
    }

    if (cfg->settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->settle_ms));
    }

    touch_reset(cfg->touch_rst_gpio, cfg->touch_int_gpio);

    if (cfg->rst_gpio >= 0) {
        gpio_reset_pin((gpio_num_t) cfg->rst_gpio);
        gpio_set_level((gpio_num_t) cfg->rst_gpio, 0);
        gpio_set_direction((gpio_num_t) cfg->rst_gpio, GPIO_MODE_OUTPUT);
        vTaskDelay(pdMS_TO_TICKS(cfg->rst_low_ms));
        gpio_set_level((gpio_num_t) cfg->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(cfg->rst_wait_ms));
        // the pin stays an output driven high
        ESP_LOGI(TAG, "Reset pulse on GPIO %d: %d ms low, %d ms wait", cfg->rst_gpio, cfg->rst_low_ms, cfg->rst_wait_ms);
    }

    *device_handle = &s_ldo;
    return 0;
}

static int panel_power_dev_deinit(void *device_handle)
{
    (void) device_handle;
    // The power enable GPIO is deliberately left alone: driving it inactive would switch off the board.
    if (s_ldo) {
        esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
    }
    return 0;
}

CUSTOM_DEVICE_IMPLEMENT(panel_power, panel_power_dev_init, panel_power_dev_deinit);
