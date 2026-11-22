/*
 * Logic of the on-board menu (landscape version of esp_extended_screen's main/include/ui_menu.h),
 * plain C without ESP-IDF. It knows the layout, turns touch events into actions and keeps the
 * state (finger, slider values, armed "turn off", idle timer). It draws nothing.
 *
 * Rows from top to bottom below a header (battery): turn off, brightness slider, volume slider,
 * audio output. Rows can be left out (uim_init flags).
 *
 *  - A button acts when the finger is lifted inside it (lifting outside cancels).
 *  - A slider follows the finger while it is down; the value is saved when the finger is lifted.
 *  - "Turn off" needs a second tap within UIM_ARM_MS; any other touch or the timeout disarms it.
 *  - The menu closes after `timeout_ms` without touch (0 = never), but not while a finger is down.
 * Times are milliseconds of any free running clock, compared with wrap-around arithmetic.
 */
#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdint.h>

#define UIM_BRIGHT_MIN  5       /* keep equal to TABLET_SETTINGS_BRIGHT_MIN */
#define UIM_ARM_MS      3000
#define UIM_MARGIN      60
#define UIM_HEADER_H    100     /* battery */
#define UIM_FOOTER_H    80      /* hint */
#define UIM_ROW_H       118
#define UIM_GAP         18
#define UIM_SLIDER_PAD  30      /* track inset inside a row */
#define UIM_MAX_ROWS    4

enum {
    UIM_ITEM_OFF = 0,
    UIM_ITEM_BRIGHT,
    UIM_ITEM_VOLUME,
    UIM_ITEM_AUDIO,             /* toggles between speaker and headphones */
};

enum { UIM_HAS_OFF = 1, UIM_HAS_AUDIO = 2 };
enum { UIM_DOWN = 1, UIM_MOVE = 2, UIM_UP = 3 };

enum {
    UIA_NONE = 0,
    UIA_REDRAW,         /* item = item to redraw, -1 = the whole page */
    UIA_BRIGHTNESS,     /* value = new brightness, redraw the row */
    UIA_VOLUME,         /* value = new volume, redraw the row */
    UIA_SAVE,           /* a slider was released: store the settings */
    UIA_POWER_OFF,
    UIA_AUDIO,          /* toggle speaker / headphones, redraw the row */
    UIA_CLOSE,          /* idle timeout */
};

typedef struct { int kind; int item; int value; } uim_action_t;

typedef struct {
    int w, h;
    int x1, x2;                 /* left and right edge of the rows */
    int row_h, gap, top;
    int rows;
    int row_item[UIM_MAX_ROWS]; /* item shown in each row */
    int active;                 /* item the finger went down on, -1 none */
    int inside;                 /* the finger is still inside that item */
    int down;
    int bright, volume;
    int armed;
    uint32_t armed_until;
    uint32_t last_ms;
    uint32_t timeout_ms;
} uim_t;

static inline int uim_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline void uim_init(uim_t *m, int w, int h, int flags, int bright, int volume, uint32_t timeout_ms,
                            uint32_t now)
{
    m->w = w;
    m->h = h;
    m->x1 = UIM_MARGIN;
    m->x2 = w - 1 - UIM_MARGIN;
    m->gap = UIM_GAP;
    m->rows = 0;
    if (flags & UIM_HAS_OFF) m->row_item[m->rows++] = UIM_ITEM_OFF;
    m->row_item[m->rows++] = UIM_ITEM_BRIGHT;
    m->row_item[m->rows++] = UIM_ITEM_VOLUME;
    if (flags & UIM_HAS_AUDIO) m->row_item[m->rows++] = UIM_ITEM_AUDIO;
    const int avail = h - UIM_HEADER_H - UIM_FOOTER_H;
    m->row_h = UIM_ROW_H;
    if (m->rows * UIM_ROW_H + (m->rows - 1) * UIM_GAP > avail) {
        m->row_h = (avail - (m->rows - 1) * UIM_GAP) / m->rows;
    }
    m->top = UIM_HEADER_H + (avail - (m->rows * m->row_h + (m->rows - 1) * m->gap)) / 2;
    m->active = -1;
    m->inside = 0;
    m->down = 0;
    m->bright = uim_clamp(bright, UIM_BRIGHT_MIN, 100);
    m->volume = uim_clamp(volume, 0, 100);
    m->armed = 0;
    m->armed_until = 0;
    m->last_ms = now;
    m->timeout_ms = timeout_ms;
}

/* Row of an item, -1 if it is not shown */
static inline int uim_row(const uim_t *m, int item)
{
    for (int i = 0; i < m->rows; i++) {
        if (m->row_item[i] == item) return i;
    }
    return -1;
}

/* Rectangle of an item (inclusive corners) */
static inline void uim_rect(const uim_t *m, int item, int *x1, int *y1, int *x2, int *y2)
{
    const int row = uim_row(m, item);
    *x1 = m->x1;
    *x2 = m->x2;
    *y1 = m->top + (row < 0 ? 0 : row) * (m->row_h + m->gap);
    *y2 = *y1 + m->row_h - 1;
}

/* Slider track (left and right end) */
static inline void uim_track(const uim_t *m, int *tx1, int *tx2)
{
    *tx1 = m->x1 + UIM_SLIDER_PAD;
    *tx2 = m->x2 - UIM_SLIDER_PAD;
}

static inline int uim_is_slider(int item) { return item == UIM_ITEM_BRIGHT || item == UIM_ITEM_VOLUME; }

/* Item under a point, -1 if none */
static inline int uim_hit(const uim_t *m, int x, int y)
{
    int x1, y1, x2, y2;
    for (int i = 0; i < m->rows; i++) {
        uim_rect(m, m->row_item[i], &x1, &y1, &x2, &y2);
        if (x >= x1 && x <= x2 && y >= y1 && y <= y2) {
            return m->row_item[i];
        }
    }
    return -1;
}

/* Slider value for a finger at x */
static inline int uim_value_at(const uim_t *m, int item, int x)
{
    int tx1, tx2;
    uim_track(m, &tx1, &tx2);
    const int vmin = (item == UIM_ITEM_BRIGHT) ? UIM_BRIGHT_MIN : 0;
    const int span = tx2 - tx1;
    x = uim_clamp(x, tx1, tx2);
    return vmin + ((x - tx1) * (100 - vmin) + span / 2) / span;
}

static inline uim_action_t uim_act(int kind, int item, int value)
{
    uim_action_t a;
    a.kind = kind;
    a.item = item;
    a.value = value;
    return a;
}

/* Sets the slider value of the item, returns the action if it changed */
static inline uim_action_t uim_slide(uim_t *m, int item, int x)
{
    const int v = uim_value_at(m, item, x);
    int *cur = (item == UIM_ITEM_BRIGHT) ? &m->bright : &m->volume;
    if (v == *cur) {
        return uim_act(UIA_NONE, item, v);
    }
    *cur = v;
    return uim_act(item == UIM_ITEM_BRIGHT ? UIA_BRIGHTNESS : UIA_VOLUME, item, v);
}

static inline uim_action_t uim_event(uim_t *m, int phase, int x, int y, uint32_t now)
{
    m->last_ms = now;

    if (phase == UIM_DOWN) {
        m->down = 1;
        const int it = uim_hit(m, x, y);
        m->active = it;
        m->inside = (it >= 0);
        const int was_armed = m->armed;
        if (it != UIM_ITEM_OFF) {
            m->armed = 0;
        }
        if (it < 0) {
            return was_armed ? uim_act(UIA_REDRAW, UIM_ITEM_OFF, 0) : uim_act(UIA_NONE, -1, 0);
        }
        if (uim_is_slider(it)) {
            uim_action_t a = uim_slide(m, it, x);
            if (a.kind == UIA_NONE && was_armed) {
                return uim_act(UIA_REDRAW, UIM_ITEM_OFF, 0);
            }
            if (was_armed) {
                return uim_act(UIA_REDRAW, -1, 0);  /* value changed and "turn off" disarmed */
            }
            return a;
        }
        if (was_armed && it != UIM_ITEM_OFF) {
            return uim_act(UIA_REDRAW, -1, 0);     /* the "turn off" row and the pressed row change */
        }
        return uim_act(UIA_REDRAW, it, 0);          /* pressed look */
    }

    if (phase == UIM_MOVE) {
        if (m->active < 0) {
            return uim_act(UIA_NONE, -1, 0);
        }
        if (uim_is_slider(m->active)) {
            return uim_slide(m, m->active, x);
        }
        const int in = (uim_hit(m, x, y) == m->active);
        if (in != m->inside) {
            m->inside = in;
            return uim_act(UIA_REDRAW, m->active, 0);
        }
        return uim_act(UIA_NONE, -1, 0);
    }

    /* UIM_UP */
    m->down = 0;
    const int it = m->active;
    m->active = -1;
    if (it < 0) {
        return uim_act(UIA_NONE, -1, 0);
    }
    if (uim_is_slider(it)) {
        return uim_act(UIA_SAVE, it, 0);
    }
    const int inside = (uim_hit(m, x, y) == it);
    m->inside = 0;
    if (!inside) {
        return uim_act(UIA_REDRAW, it, 0);          /* cancelled */
    }
    switch (it) {
    case UIM_ITEM_OFF:
        if (m->armed && (int32_t)(now - m->armed_until) < 0) {
            m->armed = 0;
            return uim_act(UIA_POWER_OFF, it, 0);
        }
        m->armed = 1;
        m->armed_until = now + UIM_ARM_MS;
        return uim_act(UIA_REDRAW, it, 0);
    case UIM_ITEM_AUDIO:
        return uim_act(UIA_AUDIO, it, 0);
    default:
        return uim_act(UIA_REDRAW, it, 0);
    }
}

/* Call about every 100 ms: expires the armed "turn off" and the idle timeout */
static inline uim_action_t uim_tick(uim_t *m, uint32_t now)
{
    if (m->armed && (int32_t)(now - m->armed_until) >= 0) {
        m->armed = 0;
        return uim_act(UIA_REDRAW, UIM_ITEM_OFF, 0);
    }
    if (m->timeout_ms && !m->down && (uint32_t)(now - m->last_ms) >= m->timeout_ms) {
        return uim_act(UIA_CLOSE, -1, 0);
    }
    return uim_act(UIA_NONE, -1, 0);
}

#endif
