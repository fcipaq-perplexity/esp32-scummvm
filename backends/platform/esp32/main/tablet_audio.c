/*
 * I2S audio output, see tablet_audio.h. Volume curve and amplifier switching as in
 * esp_extended_screen (main/app_audio_volume.c).
 */
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tablet_audio.h"

static const char *TAG = "tablet_audio";

static volatile uint32_t s_gain = 256;      /* 256 = unity */
static volatile bool s_hp;
static volatile bool s_on;

#if CONFIG_TABLET_AUDIO
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#define SPK_EN ((gpio_num_t)CONFIG_TABLET_AUDIO_GPIO_SPK_EN)
#define HP_EN  ((gpio_num_t)CONFIG_TABLET_AUDIO_GPIO_HP_EN)

static i2s_chan_handle_t s_tx;

static void amps_update(void)
{
    gpio_set_level(SPK_EN, s_on && !s_hp);
    gpio_set_level(HP_EN, s_on && s_hp);
}

esp_err_t tablet_audio_init(int sample_rate)
{
    const gpio_config_t amp_pins = {
        .pin_bit_mask = (1ULL << CONFIG_TABLET_AUDIO_GPIO_SPK_EN) | (1ULL << CONFIG_TABLET_AUDIO_GPIO_HP_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp_pins), TAG, "amplifier enable pins");
    s_on = false;
    amps_update();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;     /* silence instead of repeating old samples on underrun */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "create I2S channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)CONFIG_TABLET_AUDIO_GPIO_BCLK,
            .ws = (gpio_num_t)CONFIG_TABLET_AUDIO_GPIO_LRCLK,
            .dout = (gpio_num_t)CONFIG_TABLET_AUDIO_GPIO_DATA,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable I2S channel");
    /* Let the clocks run with silence for a moment before the amplifier comes on (no pop) */
    vTaskDelay(pdMS_TO_TICKS(20));
    s_on = true;
    amps_update();
    ESP_LOGI(TAG, "I2S %d Hz: DATA %d, BCLK %d, LRCLK %d, speaker enable %d, headphone enable %d", sample_rate,
             CONFIG_TABLET_AUDIO_GPIO_DATA, CONFIG_TABLET_AUDIO_GPIO_BCLK, CONFIG_TABLET_AUDIO_GPIO_LRCLK,
             CONFIG_TABLET_AUDIO_GPIO_SPK_EN, CONFIG_TABLET_AUDIO_GPIO_HP_EN);
    return ESP_OK;
}

bool tablet_audio_available(void)
{
    return s_tx != NULL;
}

esp_err_t tablet_audio_write(void *buf, size_t bytes)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t gain = s_gain;
    if (gain < 256) {
        int16_t *s = (int16_t *)buf;
        for (size_t i = 0, n = bytes / sizeof(int16_t); i < n; i++) {
            s[i] = (int16_t)(((int32_t)s[i] * (int32_t)gain) >> 8);
        }
    }
    size_t written = 0;
    return i2s_channel_write(s_tx, buf, bytes, &written, portMAX_DELAY);
}

void tablet_audio_set_headphones(bool headphones)
{
    s_hp = headphones;
    amps_update();
}

void tablet_audio_shutdown(void)
{
    s_gain = 0;
    vTaskDelay(pdMS_TO_TICKS(60));
    s_on = false;
    amps_update();
}

#else

esp_err_t tablet_audio_init(int sample_rate)
{
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}

bool tablet_audio_available(void)
{
    return false;
}

esp_err_t tablet_audio_write(void *buf, size_t bytes)
{
    (void)buf; (void)bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

void tablet_audio_set_headphones(bool headphones)
{
    s_hp = headphones;
}

void tablet_audio_shutdown(void)
{
}

#endif

void tablet_audio_set_volume(int percent)
{
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    s_gain = ((uint32_t)percent * (uint32_t)percent * 256u + 5000u) / 10000u;
}

bool tablet_audio_headphones(void)
{
    return s_hp;
}
