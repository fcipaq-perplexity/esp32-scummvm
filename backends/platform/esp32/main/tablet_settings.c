/* Settings file on the SD card, see tablet_settings.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "tablet_audio.h"
#include "tablet_lcd.h"
#include "tablet_settings.h"

static const char *TAG = "tablet_settings";

static tablet_settings_t s_cur = { .brightness = 80, .volume = 70, .headphones = false };
static tablet_settings_t s_saved;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

tablet_settings_t *tablet_settings(void)
{
    return &s_cur;
}

void tablet_settings_load(void)
{
    FILE *f = fopen(TABLET_SETTINGS_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "%s not found, using defaults", TABLET_SETTINGS_FILE);
        s_saved = s_cur;
        return;
    }
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const int v = atoi(eq + 1);
        if (strcmp(line, "brightness") == 0) {
            s_cur.brightness = clampi(v, TABLET_SETTINGS_BRIGHT_MIN, 100);
        } else if (strcmp(line, "volume") == 0) {
            s_cur.volume = clampi(v, 0, 100);
        } else if (strcmp(line, "headphones") == 0) {
            s_cur.headphones = v != 0;
        }
    }
    fclose(f);
    s_saved = s_cur;
    ESP_LOGI(TAG, "Loaded: brightness %d, volume %d, %s", s_cur.brightness, s_cur.volume,
             s_cur.headphones ? "headphones" : "speaker");
}

void tablet_settings_apply(void)
{
    tablet_backlight_set(s_cur.brightness);
    tablet_audio_set_volume(s_cur.volume);
    tablet_audio_set_headphones(s_cur.headphones);
}

void tablet_settings_save(void)
{
    if (memcmp(&s_cur, &s_saved, sizeof(s_cur)) == 0) {
        return;
    }
    FILE *f = fopen(TABLET_SETTINGS_FILE, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot write %s", TABLET_SETTINGS_FILE);
        return;
    }
    fprintf(f, "brightness=%d\nvolume=%d\nheadphones=%d\n", s_cur.brightness, s_cur.volume, s_cur.headphones ? 1 : 0);
    if (fclose(f) == 0) {
        s_saved = s_cur;
        ESP_LOGI(TAG, "Saved %s", TABLET_SETTINGS_FILE);
    }
}
