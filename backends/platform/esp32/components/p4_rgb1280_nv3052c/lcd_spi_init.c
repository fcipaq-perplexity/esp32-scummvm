/*
 * Board prep for the 720x1280 NV3052C RGB panel:
 *   (panel supply: see panel_power.c) -> optional hardware reset -> 9-bit bit-banged 3-wire SPI
 *   init -> release SPI pins
 *
 * The reset line (GPIO 54) is shared with the C6 co-processor and is pulsed by ESP-Hosted at
 * Wi-Fi start. The board definition sets lcd_rst_gpio to -1, so no hardware reset is done here and
 * the init sequence starts with a software reset instead.
 *
 * Ported 1:1 from the verified standalone test firmware: pin handling, delays and bit order are
 * intentionally unchanged (the panel needs this exact 9-bit mode-0 sequence). Registered as the
 * Board Manager custom device "lcd_spi_init"; display_lcd depends on it (see board_devices.yaml).
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dev_custom.h"
#include "gen_board_device_custom.h"
#include "lcd_nv3052c_init.h"

static const char *TAG = "lcd_spi_init";

static int s_handle;
static int s_sck = -1;
static int s_mosi = -1;
static int s_rst = -1;

static void spi_setup_gpios(void)
{
    gpio_reset_pin((gpio_num_t) s_sck);
    gpio_reset_pin((gpio_num_t) s_mosi);

    gpio_set_level((gpio_num_t) s_sck, 0);
    gpio_set_level((gpio_num_t) s_mosi, 0);

    gpio_set_direction((gpio_num_t) s_sck, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t) s_mosi, GPIO_MODE_OUTPUT);

    if (s_rst >= 0) {
        gpio_reset_pin((gpio_num_t) s_rst);
        gpio_set_level((gpio_num_t) s_rst, 0);
        gpio_set_direction((gpio_num_t) s_rst, GPIO_MODE_OUTPUT);
        vTaskDelay(pdMS_TO_TICKS(100));  // reset low
        gpio_set_level((gpio_num_t) s_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(200));  // wait for the display to get ready
    }

}

static void spi_deinit_gpios(void)
{
    // Some displays need these lines to be down if CS is always asserted
    gpio_set_direction((gpio_num_t) s_sck, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t) s_sck, GPIO_PULLDOWN_ONLY);

    gpio_set_direction((gpio_num_t) s_mosi, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t) s_mosi, GPIO_PULLDOWN_ONLY);
}

// Mode 0 software SPI, 9 bit frames: D/C bit first, then 8 data bits MSB first.
static void spi_send_byte(uint8_t val, int dc)
{
    gpio_set_level((gpio_num_t) s_mosi, dc == SPI_COMMAND ? 0 : 1);  // 0: cmd; 1: data

    esp_rom_delay_us(1);
    gpio_set_level((gpio_num_t) s_sck, 1);  // rising
    esp_rom_delay_us(1);
    gpio_set_level((gpio_num_t) s_sck, 0);  // falling
    esp_rom_delay_us(1);

    for (int i = 0; i < 8; i++) {
        gpio_set_level((gpio_num_t) s_mosi, (val & 0x80) ? 1 : 0);
        val <<= 1;

        esp_rom_delay_us(1);
        gpio_set_level((gpio_num_t) s_sck, 1);  // rising
        esp_rom_delay_us(1);
        gpio_set_level((gpio_num_t) s_sck, 0);  // falling
    }
}

static int spi_send(const uint8_t *init_seq)
{
    spi_send_byte(0x01, SPI_COMMAND);  // SW reset
    vTaskDelay(pdMS_TO_TICKS(200));

    int pos = 0;
    while (init_seq[pos] != END_LIST) {
        int n = 0;
        switch (init_seq[pos]) {
        case WRITE:
            pos++;
            n = init_seq[pos];
            break;
        case DELAY:
            pos++;
            vTaskDelay(pdMS_TO_TICKS(init_seq[pos]));
            break;
        default:
            ESP_LOGE(TAG, "illegal instruction 0x%02X at offset %d in LCD init sequence", init_seq[pos], pos);
            return -1;
        }

        for (int i = 0; i < n; i++) {
            pos++;
            spi_send_byte(init_seq[pos], i == 0 ? SPI_COMMAND : SPI_DATA);
        }
        pos++;
    }
    return 0;
}

static int lcd_spi_init_dev_init(void *config, int cfg_size, void **device_handle)
{
    dev_custom_lcd_spi_init_config_t *cfg = (dev_custom_lcd_spi_init_config_t *) config;
    if (cfg_size != (int) sizeof(*cfg)) {
        ESP_LOGE(TAG, "Invalid config size: %d, expected: %d", cfg_size, (int) sizeof(*cfg));
        return -1;
    }

    s_sck = cfg->spi_sck_gpio;
    s_mosi = cfg->spi_mosi_gpio;
    s_rst = cfg->lcd_rst_gpio;

    // PCLK pin must be in its reset state before the RGB peripheral takes it over
    gpio_reset_pin((gpio_num_t) cfg->pclk_gpio);

    spi_setup_gpios();
    int ret = spi_send(panel_init_operations);
    spi_deinit_gpios();
    if (ret != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "NV3052C init sequence sent");
    *device_handle = &s_handle;
    return 0;
}

static int lcd_spi_init_dev_deinit(void *device_handle)
{
    (void) device_handle;
    return 0;
}

CUSTOM_DEVICE_IMPLEMENT(lcd_spi_init, lcd_spi_init_dev_init, lcd_spi_init_dev_deinit);
