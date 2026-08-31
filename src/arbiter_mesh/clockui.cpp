#include "clockui.h"
#include "clocksrc.h"
#include <lvgl.h>
#include <Arduino.h>
#include <stdio.h>

// ── Clock setter ─────────────────────────────────────────────────────────────
//
// Buttons only: this LVGL build has no spinbox, roller or keyboard, and the
// flash budget does not want them. Six fields with big +/- targets is also the
// right shape for setting a clock outdoors with cold hands.

#define COL_BG      0x0d1117
#define COL_PANEL   0x161b22
#define COL_DIV     0x30363d
#define COL_TEXT    0xe6edf3
#define COL_DIM     0x8b949e
#define COL_TITLE   0x58a6ff
#define COL_OK      0x3fb950
#define COL_BTN     0x21262d
#define COL_CANCEL  0x484f58

static pulleys::RTC* s_rtc = nullptr;
static lv_obj_t*     s_panel   = nullptr;   // the modal
static lv_obj_t*     s_open    = nullptr;   // the corner button that opens it
static lv_obj_t*     s_valLbl[6];
static lv_obj_t*     s_liveLbl = nullptr;
static bool          s_isOpen  = false;

// year, month, day, hour, minute, second
static int s_field[6] = { 2026, 1, 1, 0, 0, 0 };
static const char* FIELD_NAME[6] = { "YEAR", "MON", "DAY", "HOUR", "MIN", "SEC" };
static const int   FIELD_MIN[6]  = { 2024, 1, 1, 0, 0, 0 };
static const int   FIELD_MAX[6]  = { 2099, 12, 31, 23, 59, 59 };

static void refreshValues() {
    char b[8];
    for (int i = 0; i < 6; i++) {
        snprintf(b, sizeof(b), i == 0 ? "%04d" : "%02d", s_field[i]);
        lv_label_set_text(s_valLbl[i], b);
    }
}

// Days in month, so 31 February cannot be dialled in and silently rejected.
static int daysInMonth(int y, int m) {
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[(m - 1) % 12];
}

static void clampDay() {
    int maxD = daysInMonth(s_field[0], s_field[1]);
    if (s_field[2] > maxD) s_field[2] = maxD;
}

static void loadFromRtc() {
    pulleys::RtcTime t;
    if (s_rtc && s_rtc->present() && s_rtc->now(t) && t.year >= 2024) {
        s_field[0] = t.year;  s_field[1] = t.month;  s_field[2] = t.day;
        s_field[3] = t.hour;  s_field[4] = t.minute; s_field[5] = t.second;
    }
    clampDay();
    refreshValues();
}

static void stepCb(lv_event_t* e) {
    intptr_t code = (intptr_t)lv_event_get_user_data(e);
    int idx = (int)(code >> 1);
    int dir = (code & 1) ? 1 : -1;
    s_field[idx] += dir;
    if (s_field[idx] > FIELD_MAX[idx]) s_field[idx] = FIELD_MIN[idx];
    if (s_field[idx] < FIELD_MIN[idx]) s_field[idx] = FIELD_MAX[idx];
    if (idx == 0 || idx == 1) clampDay();
    if (idx == 2) { int m = daysInMonth(s_field[0], s_field[1]);
                    if (s_field[2] > m) s_field[2] = 1; }
    refreshValues();
}

static void closePanel() {
    s_isOpen = false;
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_open, LV_OBJ_FLAG_HIDDEN);
}

static void openCb(lv_event_t*) {
    loadFromRtc();
    s_isOpen = true;
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_open, LV_OBJ_FLAG_HIDDEN);
}

static void cancelCb(lv_event_t*) { closePanel(); }

static void saveCb(lv_event_t*) {
    if (!s_rtc || !s_rtc->present()) { closePanel(); return; }
    pulleys::RtcTime t;
    t.year   = (uint16_t)s_field[0]; t.month  = (uint8_t)s_field[1];
    t.day    = (uint8_t)s_field[2];  t.hour   = (uint8_t)s_field[3];
    t.minute = (uint8_t)s_field[4];  t.second = (uint8_t)s_field[5];
    if (s_rtc->set(t)) {
        clocksrc_mark_set();
        char b[24]; pulleys::RTC::format(t, b, sizeof(b));
        Serial.printf("  [RTC] set from touch: %s (source now: %s)\n",
                      b, clocksrc_source_str());
    } else {
        Serial.println("  [RTC] set from touch FAILED");
    }
    closePanel();
}

static lv_obj_t* makeButton(lv_obj_t* parent, int x, int y, int w, int h,
                            const char* text, uint32_t bg,
                            lv_event_cb_t cb, void* user) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_DIV), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);
    return b;
}

void clockui_init(pulleys::RTC* rtc) {
    s_rtc = rtc;

    // The opener sits in the bottom-right corner, over the MESH quadrant's
    // spare space, so it never hides a number that matters.
    s_open = makeButton(lv_scr_act(), 800 - 132, 480 - 44, 126, 38,
                        "SET CLOCK", COL_BTN, openCb, nullptr);

    // Modal
    s_panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_panel, 760, 420);
    lv_obj_set_pos(s_panel, 20, 30);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_border_width(s_panel, 2, 0);
    lv_obj_set_style_radius(s_panel, 6, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(s_panel);
    lv_label_set_text(title, "SET CLOCK");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TITLE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);

    s_liveLbl = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_liveLbl, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(s_liveLbl, &lv_font_unscii_16, 0);
    lv_obj_align(s_liveLbl, LV_ALIGN_TOP_RIGHT, -4, 2);

    // Six stepper columns
    const int colW = 118, x0 = 6, yTop = 52;
    for (int i = 0; i < 6; i++) {
        int x = x0 + i * colW;

        lv_obj_t* name = lv_label_create(s_panel);
        lv_label_set_text(name, FIELD_NAME[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(name, &lv_font_unscii_16, 0);
        lv_obj_set_pos(name, x + 8, yTop);

        makeButton(s_panel, x, yTop + 24, 104, 62, LV_SYMBOL_UP, COL_BTN,
                   stepCb, (void*)(intptr_t)((i << 1) | 1));

        s_valLbl[i] = lv_label_create(s_panel);
        lv_obj_set_style_text_color(s_valLbl[i], lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(s_valLbl[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(s_valLbl[i], x + 34, yTop + 100);

        makeButton(s_panel, x, yTop + 128, 104, 62, LV_SYMBOL_DOWN, COL_BTN,
                   stepCb, (void*)(intptr_t)(i << 1));
    }

    lv_obj_t* hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "no coin cell: the clock is lost on power loss, so re-set it after any outage");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 4, -76);

    makeButton(s_panel, 6,   330, 200, 62, "CANCEL", COL_CANCEL, cancelCb, nullptr);
    makeButton(s_panel, 546, 330, 200, 62, "SAVE",   COL_OK,     saveCb,   nullptr);

    refreshValues();
}

void clockui_tick() {
    if (!s_isOpen || !s_liveLbl) return;
    pulleys::RtcTime t;
    char b[40];
    if (s_rtc && s_rtc->present() && s_rtc->now(t)) {
        char f[24];
        pulleys::RTC::format(t, f, sizeof(f));
        snprintf(b, sizeof(b), "now %s (%s)", f, clocksrc_source_str());
    } else {
        snprintf(b, sizeof(b), "no RTC");
    }
    lv_label_set_text(s_liveLbl, b);
}

bool clockui_is_open() { return s_isOpen; }
