/*
 * Board prep for the 720x1280 NV3052C RGB panel (board v1.2):
 *   (power and SPI line parking: see panel_power.c) -> hardware reset -> 9-bit bit-banged 3-wire SPI
 *   init -> hold or release the SPI pins
 *
 * The reset line (GPIO 54, lcd_rst_gpio) is shared with the enable pin of the C6 co-processor, so
 * the reset pulse here (100 ms low, 200 ms wait) resets both. The application therefore runs this
 * init before it starts Wi-Fi (CONFIG_EXTEND_SCREEN_LCD_BEFORE_WIFI), the C6 boots in the
 * meantime, and the ESP-Hosted reset GPIO is moved to an unused pin so nothing resets the line
 * afterwards. The pin stays an output driven high after the pulse.
 *
 * The panel's chip select is always asserted, so every clock edge on SCK counts. panel_power
 * already parks SCK and MOSI low as outputs; this file does not reset those pins again (that would
 * enable the pull-up for a moment) and, with hold_lines_low, keeps driving them low after the init
 * so that coupling from the neighbouring PCLK cannot clock garbage into the panel.
 *
 * Ported from the verified standalone test firmware: delays and bit order are unchanged except that
 * the half clock period is configurable (spi_half_period_us, 1 in the original). Registered as the
 * Board Manager custom device "lcd_spi_init"; display_lcd depends on it (see board_devices.yaml).
 */
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
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
static uint32_t s_half_us = 1;
static bool s_hold_low;

static void spi_setup_gpios(void)
{
    // no gpio_reset_pin() here: the pins are normally parked low by panel_power already
    esp_rom_gpio_pad_select_gpio((uint32_t) s_sck);
    esp_rom_gpio_pad_select_gpio((uint32_t) s_mosi);

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
    gpio_set_level((gpio_num_t) s_sck, 0);
    gpio_set_level((gpio_num_t) s_mosi, 0);
    if (s_hold_low) {
        // keep driving both lines low, much stronger than a pull-down
        return;
    }
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

    esp_rom_delay_us(s_half_us);
    gpio_set_level((gpio_num_t) s_sck, 1);  // rising
    esp_rom_delay_us(s_half_us);
    gpio_set_level((gpio_num_t) s_sck, 0);  // falling
    esp_rom_delay_us(s_half_us);

    for (int i = 0; i < 8; i++) {
        gpio_set_level((gpio_num_t) s_mosi, (val & 0x80) ? 1 : 0);
        val <<= 1;

        esp_rom_delay_us(s_half_us);
        gpio_set_level((gpio_num_t) s_sck, 1);  // rising
        esp_rom_delay_us(s_half_us);
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

#if defined(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE)
    // ESP-Hosted pulses its reset GPIO at transport init and in esp_wifi_init(). If that is one of
    // the pins this board uses for the panel, the panel is reset or disturbed after its init (the
    // reset line is also the enable pin of the C6). A stale sdkconfig of another board keeps the old
    // value, because board defaults do not override existing options.
    const int hosted_rst = CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE;
    if (hosted_rst >= 0 &&
            (hosted_rst == cfg->lcd_rst_gpio || hosted_rst == cfg->spi_sck_gpio ||
             hosted_rst == cfg->spi_mosi_gpio || hosted_rst == cfg->pclk_gpio)) {
        ESP_LOGE(TAG, "The ESP-Hosted reset GPIO %d is used by the panel (reset %d, SCK %d, MOSI %d, PCLK %d). "
                 "Set CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE to an unused GPIO (stale sdkconfig? delete it and run idf.py bmgr -b <board> again)",
                 hosted_rst, cfg->lcd_rst_gpio, cfg->spi_sck_gpio, cfg->spi_mosi_gpio, cfg->pclk_gpio);
        return -1;
    }
#endif

    s_sck = cfg->spi_sck_gpio;
    s_mosi = cfg->spi_mosi_gpio;
    s_rst = cfg->lcd_rst_gpio;
    s_half_us = cfg->spi_half_period_us > 0 ? (uint32_t) cfg->spi_half_period_us : 1;
    s_hold_low = cfg->hold_lines_low != 0;

    // PCLK pin must be in its reset state before the RGB peripheral takes it over
    gpio_reset_pin((gpio_num_t) cfg->pclk_gpio);

    ESP_LOGI(TAG, "Bit-banged SPI: half period %u us, lines %s after the init, reset GPIO %d", (unsigned) s_half_us,
             s_hold_low ? "held low" : "released", s_rst);

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
