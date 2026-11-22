/*
 * Power, battery and button of the tablet boards, see tablet_hw.h.
 *
 * Battery and power sequencing are ported from esp_extended_screen (main/app_battery.c,
 * main/include/app_power.h), which adapted them from fcipaq-perplexity/snes9x_esp32 by
 * Daniel Kammer (GPL-3.0-or-later): 2:1 divider, 12 dB attenuation, 16 samples spaced 50 us,
 * 50/51 smoothing. Charge status low means charging.
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "tablet_hw.h"
#include "tablet_ui.h"
#if CONFIG_TABLET_BATTERY
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "tablet_hw";

/* ---- Power -------------------------------------------------------------------------------- */

static void float_pin(int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void tablet_power_hold(void)
{
#if CONFIG_TABLET_POWER_OFF_GPIO >= 0
    float_pin(CONFIG_TABLET_POWER_OFF_GPIO);
#endif
#if CONFIG_TABLET_POWER_HOLD_GPIO >= 0
    /* Preload the output latch before the driver is enabled, so the pin never glitches low */
    gpio_set_level(CONFIG_TABLET_POWER_HOLD_GPIO, 1);
    gpio_set_direction(CONFIG_TABLET_POWER_HOLD_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "Power hold on GPIO %d", CONFIG_TABLET_POWER_HOLD_GPIO);
#endif
}

bool tablet_can_power_off(void)
{
    return CONFIG_TABLET_POWER_OFF_GPIO >= 0;
}

void tablet_power_off(void)
{
#if CONFIG_TABLET_POWER_OFF_GPIO >= 0
    ESP_LOGI(TAG, "Turning off: GPIO %d high", CONFIG_TABLET_POWER_OFF_GPIO);
    float_pin(CONFIG_TABLET_POWER_OFF_GPIO);
    gpio_set_level(CONFIG_TABLET_POWER_OFF_GPIO, 1);
    gpio_set_direction(CONFIG_TABLET_POWER_OFF_GPIO, GPIO_MODE_OUTPUT);
    /* The pin stays high. Do not return to normal operation, even if powered externally. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    ESP_LOGW(TAG, "This board cannot switch itself off");
#endif
}

/* ---- Battery ------------------------------------------------------------------------------ */

static portMUX_TYPE s_bat_mux = portMUX_INITIALIZER_UNLOCKED;
static tablet_battery_t s_bat;

void tablet_battery_get(tablet_battery_t *status)
{
    portENTER_CRITICAL(&s_bat_mux);
    *status = s_bat;
    portEXIT_CRITICAL(&s_bat_mux);
}

#if CONFIG_TABLET_BATTERY
#define BAT_ADC_GPIO     GPIO_NUM_23
#define BAT_CHARGE_GPIO  GPIO_NUM_22

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cal;
static adc_channel_t s_channel;
static int s_filtered_mv;

static void battery_sample(void)
{
    tablet_battery_t st = {0};
    st.charging = gpio_get_level(BAT_CHARGE_GPIO) == 0;
    int raw, sum = 0, count = 0;
    adc_oneshot_read(s_adc, s_channel, &raw);
    esp_rom_delay_us(50);
    for (int i = 0; i < 16; ++i) {
        if (adc_oneshot_read(s_adc, s_channel, &raw) == ESP_OK) {
            sum += raw;
            ++count;
        }
        esp_rom_delay_us(50);
    }
    if (count) {
        raw = sum / count;
        int pin_mv = 0;
        st.calibrated = s_cal && adc_cali_raw_to_voltage(s_cal, raw, &pin_mv) == ESP_OK;
        if (!st.calibrated) {
            pin_mv = raw * 4400 / 4095;
        }
        const int mv = pin_mv * 2;
        s_filtered_mv = s_filtered_mv ? (s_filtered_mv * 50 + mv) / 51 : mv;
        const int percent = (s_filtered_mv - 3400) * 100 / (4050 - 3400);
        st.millivolts = (uint16_t)s_filtered_mv;
        st.percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        st.valid = true;
    }
    portENTER_CRITICAL(&s_bat_mux);
    s_bat = st;
    portEXIT_CRITICAL(&s_bat_mux);
}

static void battery_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        battery_sample();
    }
}
#endif

esp_err_t tablet_battery_init(void)
{
#if CONFIG_TABLET_BATTERY
    adc_unit_t unit_id;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BAT_ADC_GPIO, &unit_id, &s_channel), TAG, "ADC channel");
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = unit_id, .ulp_mode = ADC_ULP_MODE_DISABLE };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "ADC unit");
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_channel, &chan), TAG, "ADC channel config");
    gpio_pullup_dis(BAT_ADC_GPIO);
    gpio_pulldown_dis(BAT_ADC_GPIO);
    esp_err_t ret;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = { .unit_id = unit_id, .chan = s_channel, .atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_12 };
    ret = adc_cali_create_scheme_curve_fitting(&cal, &s_cal);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = { .unit_id = unit_id, .atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_12 };
    ret = adc_cali_create_scheme_line_fitting(&cal, &s_cal);
#else
    ret = ESP_ERR_NOT_SUPPORTED;
#endif
    if (ret != ESP_OK) {
        s_cal = NULL;
        ESP_LOGW(TAG, "No ADC calibration, nominal conversion used");
    }
    gpio_config_t chg = {
        .pin_bit_mask = 1ULL << BAT_CHARGE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&chg), TAG, "charge status pin");
    battery_sample();
    BaseType_t ok = xTaskCreatePinnedToCore(battery_task, "battery", 3072, NULL, 2, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create battery task failed");
    ESP_LOGI(TAG, "Battery: VBAT GPIO 23 (divider x2), charge status GPIO 22");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ---- Menu button -------------------------------------------------------------------------- */

#if CONFIG_TABLET_MENU_BUTTON_GPIO >= 0
#define BUTTON_LOCKOUT_US 300000

/* A level must be stable for 4 samples (40 ms); after a press, presses are ignored for 300 ms */
static void button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_TABLET_MENU_BUTTON_GPIO;
    bool stable = gpio_get_level(pin), candidate = stable;
    int samples = 0;
    int64_t last_press = -BUTTON_LOCKOUT_US;
    for (;;) {
        const bool raw = gpio_get_level(pin);
        if (raw != candidate) {
            candidate = raw;
            samples = 1;
        } else if (samples < 4) {
            ++samples;
        }
        const int64_t now = esp_timer_get_time();
        if (samples >= 4 && stable != candidate) {
            stable = candidate;
            if (stable && now - last_press >= BUTTON_LOCKOUT_US) {
                last_press = now;
                ESP_LOGI(TAG, "Button pressed: toggle menu");
                tablet_ui_toggle();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

esp_err_t tablet_button_init(void)
{
#if CONFIG_TABLET_MENU_BUTTON_GPIO >= 0
    /* Active high, external divider: no internal pulls */
    float_pin(CONFIG_TABLET_MENU_BUTTON_GPIO);
    BaseType_t ok = xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create button task failed");
    ESP_LOGI(TAG, "Menu button on GPIO %d", CONFIG_TABLET_MENU_BUTTON_GPIO);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
