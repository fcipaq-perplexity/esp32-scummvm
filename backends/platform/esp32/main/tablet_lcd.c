/*
 * Display, backlight and touch of the tablet boards, see tablet_lcd.h.
 *
 * Framebuffers: the RGB panel driver (components/esp_lcd) owns two PSRAM framebuffers and feeds
 * the panel from them through bounce buffers. One framebuffer is on screen, the PPA writes the next
 * picture into the other one (s_back), then esp_lcd_panel_draw_bitmap() with that framebuffer only
 * flips. The old one is still read by the bounce buffer ISR until the frame ends
 * (on_frame_buf_complete), so the next picture waits for that before it is written there.
 *
 * Rotation: landscape picture (x, y), TABLET_W x TABLET_H, to panel (px, py), 720 x 1280:
 *   90 degrees counter-clockwise (default):  px = y,              py = TABLET_W - 1 - x
 *   270 degrees (CONFIG_TABLET_ROTATE_270):  px = TABLET_H - 1 - y, py = x
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_board_manager_includes.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "tablet_lcd.h"
#include "tablet_ui.h"
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
#include "esp_lcd_touch.h"
#endif

static const char *TAG = "tablet_lcd";

#if CONFIG_TABLET_ROTATE_270
#define ROTATION_DEG 270
#else
#define ROTATION_DEG 90
#endif

#define PANEL_POWER_DEVICE  "panel_power"
#define LCD_INIT_CORE       1
#define FLIP_TIMEOUT_MS     100
#define TOUCH_TASK_CORE     0
#define TOUCH_POLL_MS       20      /* the GT911 gives false reports when read faster */
#define TOUCH_MAX           5

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];
static size_t s_fb_size;
static int s_pw, s_ph;                  /* panel size, 720 x 1280 */
static int s_back;                      /* framebuffer that is not on screen */
static uint32_t s_fb_geom[2];           /* letterbox geometry drawn into each framebuffer, 0 = unknown */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_flip_sem;
static volatile bool s_flip_pending;
static bool s_flip_timeout_logged;
static ppa_client_handle_t s_ppa;
static bool s_ui_active, s_ui_shown;
static esp_err_t s_init_ret = ESP_FAIL;

static IRAM_ATTR bool on_frame_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    if (!s_flip_pending) {
        return false;
    }
    s_flip_pending = false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flip_sem, &woken);
    return woken == pdTRUE;
}

/* The framebuffer s_back may be written once the last flip is on screen */
static void wait_flip(void)
{
    if (!s_flip_pending) {
        return;
    }
    if (xSemaphoreTake(s_flip_sem, pdMS_TO_TICKS(FLIP_TIMEOUT_MS)) != pdTRUE) {
        s_flip_pending = false;
        if (!s_flip_timeout_logged) {
            s_flip_timeout_logged = true;
            ESP_LOGW(TAG, "No frame complete event within %d ms (logged once)", FLIP_TIMEOUT_MS);
        }
    }
}

static void flip(void)
{
    xSemaphoreTake(s_flip_sem, 0);      /* drop a stale give */
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_pw, s_ph, s_fb[s_back]);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "flip failed: %s", esp_err_to_name(ret));
        return;
    }
    s_flip_pending = true;
    s_back ^= 1;
}

void tablet_lcd_viewport(int w, int h, int *ox, int *oy, int *ow, int *oh)
{
    if (w <= 0 || h <= 0) {
        *ox = *oy = 0; *ow = TABLET_W; *oh = TABLET_H;
        return;
    }
    /* The PPA scales in steps of 1/16 */
    int s16 = TABLET_W * 16 / w;
    if (TABLET_H * 16 / h < s16) {
        s16 = TABLET_H * 16 / h;
    }
    if (s16 < 1) {
        s16 = 1;
    }
    *ow = w * s16 / 16;
    *oh = h * s16 / 16;
    *ox = (TABLET_W - *ow) / 2;
    *oy = (TABLET_H - *oh) / 2;
}

/* PPA: src (w x h, landscape) scaled by s16/16 into the panel framebuffer fb, rotated */
static esp_err_t ppa_to_panel(const uint16_t *src, int w, int h, uint16_t *fb, int ox, int oy, int ow, int oh,
                              int s16)
{
    ppa_srm_oper_config_t op;
    memset(&op, 0, sizeof(op));
    op.in.buffer = src;
    op.in.pic_w = w;
    op.in.pic_h = h;
    op.in.block_w = w;
    op.in.block_h = h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = fb;
    op.out.buffer_size = s_fb_size;
    op.out.pic_w = s_pw;
    op.out.pic_h = s_ph;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#if CONFIG_TABLET_ROTATE_270
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    op.out.block_offset_x = TABLET_H - oy - oh;
    op.out.block_offset_y = ox;
#else
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.out.block_offset_x = oy;
    op.out.block_offset_y = TABLET_W - ox - ow;
#endif
    op.scale_x = (float)s16 / 16.0f;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

static void clear_fb(int i)
{
    memset(s_fb[i], 0, s_fb_size);
    esp_cache_msync(s_fb[i], s_fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* Draws a picture into the back framebuffer (letterboxed) */
static esp_err_t render_back(const uint16_t *src, int w, int h)
{
    int ox, oy, ow, oh;
    tablet_lcd_viewport(w, h, &ox, &oy, &ow, &oh);
    const int s16 = ow * 16 / w;
    const uint32_t geom = ((uint32_t)ow << 16) | (uint32_t)oh;
    if (s_fb_geom[s_back] != geom) {
        clear_fb(s_back);               /* black bars around the new picture size */
        s_fb_geom[s_back] = geom;
    }
    return ppa_to_panel(src, w, h, s_fb[s_back], ox, oy, ow, oh, s16);
}

esp_err_t tablet_lcd_present(const uint16_t *src, int w, int h)
{
    if (!s_panel || !src) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_ui_active) {
        wait_flip();
        ret = render_back(src, w, h);
        if (ret == ESP_OK) {
            flip();
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t tablet_lcd_scale_to_screen(const uint16_t *src, int w, int h, uint16_t *dst)
{
    if (!s_ppa || !src || !dst) {
        return ESP_ERR_INVALID_STATE;
    }
    int ox, oy, ow, oh;
    tablet_lcd_viewport(w, h, &ox, &oy, &ow, &oh);
    const size_t size = (size_t)TABLET_W * TABLET_H * sizeof(uint16_t);
    memset(dst, 0, size);
    esp_cache_msync(dst, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ppa_srm_oper_config_t op;
    memset(&op, 0, sizeof(op));
    op.in.buffer = src;
    op.in.pic_w = w;
    op.in.pic_h = h;
    op.in.block_w = w;
    op.in.block_h = h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = dst;
    op.out.buffer_size = size;
    op.out.pic_w = TABLET_W;
    op.out.pic_h = TABLET_H;
    op.out.block_offset_x = ox;
    op.out.block_offset_y = oy;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = (float)(ow * 16 / w) / 16.0f;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa, &op);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t tablet_lcd_ui_begin(void)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = s_ui_active ? ESP_ERR_INVALID_STATE : ESP_OK;
    s_ui_active = true;
    s_ui_shown = false;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t tablet_lcd_ui_present(const uint16_t *src)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_ui_active) {
        if (!s_ui_shown) {
            /* First picture: into the free framebuffer, the game picture stays in the other one */
            wait_flip();
            ret = ppa_to_panel(src, TABLET_W, TABLET_H, s_fb[s_back], 0, 0, TABLET_W, TABLET_H, 16);
            s_fb_geom[s_back] = 0;
            if (ret == ESP_OK) {
                flip();
                s_ui_shown = true;
            }
        } else {
            /* Updates go straight into the framebuffer on screen */
            ret = ppa_to_panel(src, TABLET_W, TABLET_H, s_fb[s_back ^ 1], 0, 0, TABLET_W, TABLET_H, 16);
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void tablet_lcd_ui_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ui_active && s_ui_shown) {
        wait_flip();
        flip();                         /* back to the framebuffer with the game picture */
    }
    s_ui_active = false;
    s_ui_shown = false;
    xSemaphoreGive(s_lock);
}

/* ---- Backlight ---------------------------------------------------------------------------- */

#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
static periph_ledc_handle_t *s_ledc;
static uint32_t s_max_duty;
#endif

esp_err_t tablet_backlight_set(int percent)
{
#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
    if (!esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS)) {
        return ESP_OK;
    }
    if (!s_ledc) {
        ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS),
                            TAG, "initialize LCD brightness device failed");
        periph_ledc_handle_t *handle = NULL;
        ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS,
                                                                (void **)&handle),
                            TAG, "get LCD brightness handle failed");
        dev_ledc_ctrl_config_t *brightness_config = NULL;
        ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS,
                                                                (void **)&brightness_config),
                            TAG, "get LCD brightness config failed");
        periph_ledc_config_t *ledc_config = NULL;
        ESP_RETURN_ON_ERROR(esp_board_manager_get_periph_config(brightness_config->ledc_name,
                                                                (void **)&ledc_config),
                            TAG, "get LCD backlight LEDC config failed");
        s_max_duty = (1U << ledc_config->duty_resolution) - 1;
        s_ledc = handle;
    }
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const uint32_t duty = (uint32_t)percent * s_max_duty / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(s_ledc->speed_mode, s_ledc->channel, duty), TAG, "set backlight duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(s_ledc->speed_mode, s_ledc->channel), TAG, "update backlight duty");
#else
    (void)percent;
#endif
    return ESP_OK;
}

/* ---- Init --------------------------------------------------------------------------------- */

static esp_err_t lcd_init_impl(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    ESP_LOGE(TAG, "Selected Board Manager board has no display_lcd device");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (esp_board_manager_check_name(PANEL_POWER_DEVICE)) {
        esp_err_t ret = esp_board_manager_init_device_by_name(PANEL_POWER_DEVICE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s init failed: %s", PANEL_POWER_DEVICE, esp_err_to_name(ret));
        }
    }
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD),
                        TAG, "initialize display_lcd failed");
    dev_display_lcd_handles_t *display = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&display),
                        TAG, "get display_lcd handle failed");
    ESP_RETURN_ON_FALSE(display && display->panel_handle, ESP_ERR_INVALID_STATE, TAG, "invalid panel handle");
    dev_display_lcd_config_t *cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&cfg),
                        TAG, "get display_lcd config failed");
    s_pw = cfg->lcd_width;
    s_ph = cfg->lcd_height;
    ESP_RETURN_ON_FALSE(s_pw == TABLET_H && s_ph == TABLET_W, ESP_ERR_INVALID_SIZE, TAG,
                        "panel is %dx%d, expected %dx%d (portrait)", s_pw, s_ph, TABLET_H, TABLET_W);
    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(display->panel_handle, 2, &fb0, &fb1), TAG,
                        "the panel needs two framebuffers (num_fbs: 2)");
    ESP_RETURN_ON_FALSE(fb0 && fb1, ESP_ERR_INVALID_STATE, TAG, "no framebuffers");
    s_fb[0] = (uint16_t *)fb0;
    s_fb[1] = (uint16_t *)fb1;
    s_fb_size = (size_t)s_pw * s_ph * sizeof(uint16_t);
    clear_fb(0);
    clear_fb(1);

    const esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame_done };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(display->panel_handle, &cbs, NULL), TAG,
                        "register frame callback failed");

    /* Framebuffer 0 is on screen after the init */
    s_back = 1;
    s_panel = display->panel_handle;
    ESP_LOGI(TAG, "Panel %dx%d (%s), framebuffers %p %p, landscape %dx%d rotated by %d degrees",
             s_pw, s_ph, cfg->sub_type, fb0, fb1, TABLET_W, TABLET_H, ROTATION_DEG);
    return ESP_OK;
#endif
}

static void lcd_init_task(void *arg)
{
    s_init_ret = lcd_init_impl();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

esp_err_t tablet_lcd_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_flip_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_lock && s_flip_sem, ESP_ERR_NO_MEM, TAG, "no memory for locks");
    tablet_backlight_set(0);

    /* The panel interrupts are allocated on the core that runs the init */
    BaseType_t ok = xTaskCreatePinnedToCore(lcd_init_task, "lcd_init", 8192, xTaskGetCurrentTaskHandle(), 5, NULL,
                                            LCD_INIT_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create LCD init task failed");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_RETURN_ON_ERROR(s_init_ret, TAG, "LCD init failed");

    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa), TAG, "register PPA client failed");
    return ESP_OK;
}

/* ---- Touch -------------------------------------------------------------------------------- */

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_touch_cnt;
static uint16_t s_touch_x[TOUCH_MAX], s_touch_y[TOUCH_MAX];

int tablet_touch_get(uint16_t *x, uint16_t *y, int max)
{
    portENTER_CRITICAL(&s_touch_mux);
    int cnt = s_touch_cnt;
    for (int i = 0; i < cnt && i < max; i++) {
        x[i] = s_touch_x[i];
        y[i] = s_touch_y[i];
    }
    portEXIT_CRITICAL(&s_touch_mux);
    return cnt;
}

#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
static esp_lcd_touch_handle_t s_tp;
static uint16_t s_touch_w, s_touch_h;   /* controller coordinate range */

static uint16_t scale_clamp(uint32_t v, uint32_t src, uint32_t dst)
{
    uint32_t r = (src == 0 || src == dst) ? v : v * dst / src;
    return (uint16_t)(r >= dst ? dst - 1 : r);
}

static void touch_task(void *arg)
{
    (void)arg;
    esp_lcd_touch_point_data_t raw[TOUCH_MAX];
    uint16_t lx[TOUCH_MAX], ly[TOUCH_MAX];
    while (1) {
        uint8_t cnt = 0;
        if (esp_lcd_touch_read_data(s_tp) == ESP_OK && esp_lcd_touch_get_data(s_tp, raw, &cnt, TOUCH_MAX) == ESP_OK) {
            if (cnt > TOUCH_MAX) {
                cnt = TOUCH_MAX;
            }
            for (int i = 0; i < cnt; i++) {
                const uint16_t px = scale_clamp(raw[i].x, s_touch_w, s_pw);
                const uint16_t py = scale_clamp(raw[i].y, s_touch_h, s_ph);
#if CONFIG_TABLET_ROTATE_270
                lx[i] = py;
                ly[i] = (uint16_t)(TABLET_H - 1 - px);
#else
                lx[i] = (uint16_t)(TABLET_W - 1 - py);
                ly[i] = px;
#endif
            }
            if (tablet_ui_touch(cnt, lx, ly)) {
                cnt = 0;                /* the menu has the contacts */
            }
            portENTER_CRITICAL(&s_touch_mux);
            s_touch_cnt = cnt;
            memcpy(s_touch_x, lx, sizeof(lx));
            memcpy(s_touch_y, ly, sizeof(ly));
            portEXIT_CRITICAL(&s_touch_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}
#endif

esp_err_t tablet_touch_init(void)
{
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    ESP_RETURN_ON_FALSE(esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH), ESP_ERR_NOT_FOUND, TAG,
                        "board has no lcd_touch device");
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH), TAG,
                        "initialize lcd_touch failed");
    dev_lcd_touch_handles_t *touch = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, (void **)&touch), TAG,
                        "get lcd_touch handle failed");
    ESP_RETURN_ON_FALSE(touch && touch->touch_handle, ESP_ERR_INVALID_STATE, TAG, "invalid touch handle");
    s_tp = touch->touch_handle;
    dev_lcd_touch_config_t *cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, (void **)&cfg), TAG,
                        "get lcd_touch config failed");
    s_touch_w = cfg->touch_config.x_max;
    s_touch_h = cfg->touch_config.y_max;
    /* Core 0: keep the I2C interrupt away from the LCD interrupts on core 1 */
    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 5, NULL, TOUCH_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create touch task failed");
    ESP_LOGI(TAG, "Touch controller %ux%u", s_touch_w, s_touch_h);
    return ESP_OK;
#else
    return ESP_ERR_NOT_FOUND;
#endif
}
