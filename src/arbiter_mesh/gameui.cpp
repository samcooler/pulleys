#include "gameui.h"
#include "gamestate.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

#define COL_PANEL   0x161b22
#define COL_DIV     0x30363d
#define COL_TEXT    0xe6edf3
#define COL_DIM     0x8b949e
#define COL_TITLE   0x58a6ff
#define COL_BTN     0x21262d
#define COL_STATE   0xbc8cff   // the current state, as the GAME strip shows it
#define COL_WARN    0xd29922
#define COL_CANCEL  0x484f58

// A 4-column grid of big buttons: easy to hit with a finger in the dark.
#define MAX_STATES  24
#define GRID_COLS   4
#define BTN_W       185
#define BTN_H       64
#define GAP         8
#define GRID_Y      40

static lv_obj_t* s_open;
static lv_obj_t* s_modal;
static lv_obj_t* s_status;
static lv_obj_t* s_btn[MAX_STATES];
static uint8_t   s_count = 0;
static bool      s_isOpen = false;
static char      s_shown[32] = "";   // the state the highlight was painted for

static lv_obj_t* makeButton(lv_obj_t* parent, int x, int y, int w, int h,
                            const char* text, uint32_t bg, lv_event_cb_t cb, void* user) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_DIV), 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w - 8);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

static void close() {
    s_isOpen = false;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void paintCurrent() {
    const char* cur = gamestate_state();
    strncpy(s_shown, cur ? cur : "", sizeof(s_shown) - 1);
    for (uint8_t i = 0; i < s_count; i++) {
        const char* name = gamestate_state_name(i);
        bool on = name && cur && strcmp(name, cur) == 0;
        lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(on ? COL_STATE : COL_BTN), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_btn[i], 0),
                                    lv_color_hex(on ? COL_PANEL : COL_TEXT), 0);
    }
}

static void stateCb(lv_event_t* e) {
    uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    const char* name = gamestate_state_name(i);
    if (name && gamestate_goto(name)) {
        Serial.printf("[ui] STATES: jumped to %s\n", name);
        close();
    } else {
        lv_label_set_text(s_status, "That state is gone (a new game was loaded). Close and reopen.");
    }
}

static void restartCb(lv_event_t*) {
    if (!gamestate_ready()) return;
    gamestate_reset();
    Serial.println("[ui] STATES: restarted the game");
    close();
}

static void closeCb(lv_event_t*) { close(); }

// Rebuilt on every open, so a game loaded since (with other states) is what shows.
static void build() {
    for (uint8_t i = 0; i < s_count; i++) lv_obj_del(s_btn[i]);
    s_count = 0;
    uint8_t n = gamestate_state_count();
    if (n > MAX_STATES) n = MAX_STATES;
    for (uint8_t i = 0; i < n; i++) {
        int col = i % GRID_COLS, row = i / GRID_COLS;
        s_btn[i] = makeButton(s_modal, col * (BTN_W + GAP), GRID_Y + row * (BTN_H + GAP),
                              BTN_W, BTN_H, gamestate_state_name(i), COL_BTN,
                              stateCb, (void*)(uintptr_t)i);
        lv_obj_set_style_text_font(lv_obj_get_child(s_btn[i], 0), &lv_font_montserrat_16, 0);
    }
    s_count = n;
    if (!gamestate_ready())
        lv_label_set_text(s_status, "No game loaded.");
    else if (gamestate_state_count() > MAX_STATES)
        lv_label_set_text(s_status, "More states than fit; only the first 24 are shown.");
    else
        lv_label_set_text(s_status, "Tap a state to jump the game there. Its entry cues and timers run.");
    paintCurrent();
}

static void openCb(lv_event_t*) {
    build();
    s_isOpen = true;
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}

void gameui_init() {
    // Top right of the GAME strip (which starts at y 380), right of the trigger
    // bar, which ui.cpp shortens to leave this room.
    s_open = makeButton(lv_scr_act(), 800 - 7 - 112, 380 + 7, 112, 26, "STATES", COL_BTN, openCb, nullptr);

    s_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_modal, 800, 480);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(s_modal, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_border_width(s_modal, 2, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 10, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* t = lv_label_create(s_modal);
    lv_label_set_text(t, "SET GAME STATE");
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(t, 0, 0);

    s_status = lv_label_create(s_modal);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_unscii_8, 0);
    lv_obj_set_pos(s_status, 200, 6);

    // Bottom row, away from the grid: RESTART (to `start`) and CLOSE.
    makeButton(s_modal, 0, 480 - 20 - 56, 185, 56, "RESTART GAME", COL_WARN, restartCb, nullptr);
    makeButton(s_modal, 776 - 185, 480 - 20 - 56, 185, 56, "CLOSE", COL_CANCEL, closeCb, nullptr);
}

void gameui_tick() {
    if (!s_isOpen) return;
    const char* cur = gamestate_state();
    if (strcmp(s_shown, cur ? cur : "") != 0) paintCurrent();   // the game moved on its own
}

bool gameui_is_open() { return s_isOpen; }
