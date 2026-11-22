/*
 * On-board menu, ported from esp_extended_screen (main/app_ui.cpp) and turned into landscape.
 * All drawing is owned by the UI task; touch and button presses are queued to it.
 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tablet_audio.h"
#include "tablet_hw.h"
#include "tablet_lcd.h"
#include "tablet_settings.h"
#include "tablet_ui.h"
#include "ui_menu.h"
#include "ui_gfx.h"

static const char *TAG = "tablet_ui";

#define UI_TASK_CORE 0
#define UI_TASK_PRIORITY 4
#define UI_TICK_MS 100

#define RGB(r, g, b) ((color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BG RGB(20, 24, 36)
#define C_ROW RGB(45, 52, 70)
#define C_PRESSED RGB(75, 86, 115)
#define C_ARMED RGB(170, 30, 30)
#define C_BORDER RGB(110, 118, 140)
#define C_TEXT RGB(255, 255, 255)
#define C_HINT RGB(170, 175, 190)
#define C_TRACK RGB(25, 28, 40)
#define C_FILL RGB(0, 140, 230)
#define C_CHARGING RGB(60, 190, 90)

static font_t *const UI_FONT = (font_t *)dejavu_sans_book_28;

enum { MSG_BUTTON = 1, MSG_TOUCH = 2 };
typedef struct { uint8_t kind, phase; int16_t x, y; } ui_msg_t;

static QueueHandle_t s_queue;
static volatile bool s_open;
static volatile bool s_wait_release;
static bool s_down;
static int16_t s_last_x, s_last_y;
static uim_t s_m;
static gbuffer_t s_g;
static uint16_t *s_off;
static int s_battery_drawn = -1;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void text_centered(int cx, int y, color_t color, const char *s)
{
    font_write_string_centered(cx, y, color, (char *)s, UI_FONT, s_g);
}

static bool draw_battery_header(bool force)
{
#if CONFIG_TABLET_BATTERY
    tablet_battery_t st;
    tablet_battery_get(&st);
    const int key = (st.valid ? st.percent : 128) | (st.charging ? 256 : 0);
    if (!force && key == s_battery_drawn) return false;
    s_battery_drawn = key;
    draw_rect_fill(0, 0, s_m.w - 1, UIM_HEADER_H - 1, C_BG, s_g);
    const color_t color = st.charging ? C_CHARGING : C_TEXT;
    const int bx = UIM_MARGIN;
    draw_rect(bx, 32, bx + 60, 65, color, s_g);
    draw_rect_fill(bx + 61, 42, bx + 67, 55, color, s_g);
    if (st.valid) {
        const int fill = st.percent * 56 / 100;
        if (fill > 0) draw_rect_fill(bx + 2, 34, bx + 2 + fill, 63, color, s_g);
    }
    char label[16];
    if (st.valid) snprintf(label, sizeof(label), "%u %%", (unsigned)st.percent);
    else snprintf(label, sizeof(label), "-- %%");
    font_write_string(bx + 85, 27, color, label, UI_FONT, s_g);
    return true;
#else
    if (!force) return false;
    draw_rect_fill(0, 0, s_m.w - 1, UIM_HEADER_H - 1, C_BG, s_g);
    return true;
#endif
}

static void draw_row(int item)
{
    int x1, y1, x2, y2;
    uim_rect(&s_m, item, &x1, &y1, &x2, &y2);
    const int fh = font_get_height(UI_FONT);
    const bool pressed = s_m.active == item && s_m.inside;
    color_t bg = pressed ? C_PRESSED : C_ROW;
    if (item == UIM_ITEM_OFF && s_m.armed) bg = C_ARMED;
    draw_rect_fill(x1, y1, x2, y2, bg, s_g);
    draw_rect(x1, y1, x2, y2, C_BORDER, s_g);
    if (!uim_is_slider(item)) {
        const char *label = "";
        switch (item) {
        case UIM_ITEM_OFF: label = s_m.armed ? "Tap again to turn off" : "Turn off"; break;
        case UIM_ITEM_AUDIO: label = tablet_audio_headphones() ? "Audio: headphones" : "Audio: speaker"; break;
        default: break;
        }
        text_centered((x1 + x2) / 2, y1 + (s_m.row_h - fh) / 2, C_TEXT, label);
        return;
    }
    const bool bright = item == UIM_ITEM_BRIGHT;
    const int value = bright ? s_m.bright : s_m.volume;
    const int vmin = bright ? UIM_BRIGHT_MIN : 0;
    char pct[16];
    snprintf(pct, sizeof(pct), "%d %%", value);
    font_write_string(x1 + 24, y1 + 10, C_TEXT, (char *)(bright ? "Brightness" : "Volume"), UI_FONT, s_g);
    font_write_string(x2 - 24 - font_get_string_width(pct, UI_FONT), y1 + 10, C_TEXT, pct, UI_FONT, s_g);
    int tx1, tx2;
    uim_track(&s_m, &tx1, &tx2);
    const int hx = tx1 + (value - vmin) * (tx2 - tx1) / (100 - vmin);
    const int ty = y1 + s_m.row_h - 34;
    draw_rect_fill(tx1, ty - 7, tx2, ty + 6, C_TRACK, s_g);
    draw_rect_fill(tx1, ty - 7, hx, ty + 6, C_FILL, s_g);
    draw_rect_fill(hx - 14, ty - 24, hx + 14, ty + 24, C_TEXT, s_g);
}

static void draw_page(void)
{
    draw_rect_fill(0, 0, s_m.w - 1, s_m.h - 1, C_BG, s_g);
    draw_battery_header(true);
    for (int i = 0; i < s_m.rows; ++i) draw_row(s_m.row_item[i]);
    text_centered(s_m.w / 2, s_m.h - UIM_FOOTER_H + 20, C_HINT, "Press the button to close");
}

static void show(void)
{
    esp_err_t ret = tablet_lcd_ui_present(s_off);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Cannot show the menu: %s", esp_err_to_name(ret));
}

static void ui_close(void)
{
    tablet_settings_save();
    tablet_lcd_ui_end();
    s_open = false;
    s_down = false;
    s_wait_release = true;
    ESP_LOGI(TAG, "Menu closed");
}

static void ui_open(void)
{
    if (tablet_lcd_ui_begin() != ESP_OK) return;
    const tablet_settings_t *st = tablet_settings();
    int flags = 0;
    if (tablet_can_power_off()) flags |= UIM_HAS_OFF;
    if (tablet_audio_available()) flags |= UIM_HAS_AUDIO;
    uim_init(&s_m, TABLET_W, TABLET_H, flags, st->brightness, st->volume,
             (uint32_t)CONFIG_TABLET_UI_TIMEOUT_S * 1000u, now_ms());
    draw_page();
    s_down = false;
    s_wait_release = true;
    s_open = true;
    show();
    ESP_LOGI(TAG, "Menu open");
}

static void power_off(void)
{
    ESP_LOGI(TAG, "Turning off");
    tablet_settings_save();
    tablet_backlight_set(0);
    tablet_audio_shutdown();
    tablet_power_off();                 /* does not return on boards that can switch off */
    tablet_settings_apply();
    ui_close();
}

static void handle(uim_action_t a)
{
    tablet_settings_t *st = tablet_settings();
    switch (a.kind) {
    case UIA_REDRAW:
        if (a.item < 0) draw_page();
        else draw_row(a.item);
        show();
        break;
    case UIA_BRIGHTNESS:
        st->brightness = a.value;
        tablet_backlight_set(a.value);
        draw_row(a.item);
        show();
        break;
    case UIA_VOLUME:
        st->volume = a.value;
        tablet_audio_set_volume(a.value);
        draw_row(a.item);
        show();
        break;
    case UIA_SAVE:
        tablet_settings_save();
        break;
    case UIA_AUDIO:
        st->headphones = !tablet_audio_headphones();
        tablet_audio_set_headphones(st->headphones);
        ESP_LOGI(TAG, "Audio output: %s", st->headphones ? "headphones" : "speaker");
        draw_row(a.item);
        show();
        break;
    case UIA_POWER_OFF:
        power_off();
        break;
    case UIA_CLOSE:
        ESP_LOGI(TAG, "Menu idle, closing");
        ui_close();
        break;
    default:
        break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_msg_t msg;
    while (1) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(UI_TICK_MS)) == pdTRUE) {
            if (msg.kind == MSG_BUTTON) {
                if (s_open) ui_close();
                else ui_open();
                continue;
            }
            if (s_open) handle(uim_event(&s_m, msg.phase, msg.x, msg.y, now_ms()));
        }
        if (s_open) {
            handle(uim_tick(&s_m, now_ms()));
            if (s_open && draw_battery_header(false)) show();
        }
    }
}

extern "C" esp_err_t tablet_ui_init(void)
{
    s_off = (uint16_t *)heap_caps_aligned_alloc(128, (size_t)TABLET_W * TABLET_H * sizeof(uint16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_off, ESP_ERR_NO_MEM, TAG, "no memory for the menu buffer");
    ESP_RETURN_ON_FALSE(gbuf_create(&s_g, TABLET_W, TABLET_H, s_off) == BUF_SUCCESS, ESP_ERR_NO_MEM, TAG,
                        "menu gbuffer");
    s_queue = xQueueCreate(16, sizeof(ui_msg_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "create UI queue failed");
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, UI_TASK_PRIORITY, NULL, UI_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create UI task failed");
    return ESP_OK;
}

extern "C" void tablet_ui_toggle(void)
{
    if (!s_queue) return;
    ui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = MSG_BUTTON;
    xQueueSend(s_queue, &msg, 0);
}

extern "C" bool tablet_ui_active(void) { return s_open; }

static void post_touch(int phase, int16_t x, int16_t y)
{
    ui_msg_t msg;
    msg.kind = MSG_TOUCH;
    msg.phase = (uint8_t)phase;
    msg.x = x;
    msg.y = y;
    xQueueSend(s_queue, &msg, phase == UIM_MOVE ? 0 : pdMS_TO_TICKS(20));
}

extern "C" bool tablet_ui_touch(int cnt, const uint16_t *x, const uint16_t *y)
{
    if (!s_queue || (!s_open && !s_wait_release)) {
        s_down = false;
        return false;
    }
    if (s_wait_release) {
        if (cnt == 0) s_wait_release = false;
        return true;
    }
    if (cnt > 0) {
        const int16_t px = (int16_t)x[0], py = (int16_t)y[0];
        if (!s_down) {
            s_down = true;
            post_touch(UIM_DOWN, px, py);
        } else if (px != s_last_x || py != s_last_y) {
            post_touch(UIM_MOVE, px, py);
        }
        s_last_x = px;
        s_last_y = py;
    } else if (s_down) {
        s_down = false;
        post_touch(UIM_UP, s_last_x, s_last_y);
    }
    return true;
}
