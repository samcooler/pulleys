#include "installui.h"
#include "installcfg.h"
#include "monitor.h"
#include <lvgl.h>
#include <Arduino.h>
#include <FastLED.h>
#include <pulleys_channel.h>
#include <stdio.h>
#include <string.h>

#define COL_BG      0x0d1117
#define COL_PANEL   0x161b22
#define COL_DIV     0x30363d
#define COL_TEXT    0xe6edf3
#define COL_DIM     0x8b949e
#define COL_TITLE   0x58a6ff
#define COL_OK      0x3fb950
#define COL_WARN    0xd29922
#define COL_BTN     0x21262d
#define COL_CANCEL  0x484f58

// Channels with a button. The install runs ~20 ropes; 24 is a 6x4 grid with
// room to spare. Channel 0 is left off: it is the fault channel (see
// pulleys_install.h), never one to put a rope on.
#define UI_CHANNELS  24
#define GRID_COLS    6

#define LISTEN_MS    5000    // the window for pulling ropes that share a channel
#define MAX_CAPTURE  10

// ── Shared ────────────────────────────────────────────────────────────────────
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
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

// Full screen, so nothing underneath can be tapped while it is up.
static lv_obj_t* makeModal(const char* title) {
    lv_obj_t* m = lv_obj_create(lv_scr_act());
    lv_obj_set_size(m, 800, 480);
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(m, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_set_style_pad_all(m, 10, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* t = lv_label_create(m);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(t, 0, 0);
    return m;
}

static lv_obj_t* makeLabel(lv_obj_t* parent, int x, int y, const lv_font_t* font,
                           uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void setTextIfChanged(lv_obj_t* label, const char* text) {
    const char* cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

// A channel button wears its channel's colour, with whichever of dark or light
// text reads on it — pure blue under black text is unreadable.
static void paintChannelButton(lv_obj_t* b, uint8_t ch) {
    CRGB c = pulleys::channel_color(ch);
    lv_obj_set_style_bg_color(b, lv_color_make(c.r, c.g, c.b), 0);
    uint32_t luma = (c.r * 299u + c.g * 587u + c.b * 114u) / 1000u;
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0),
                                lv_color_hex(luma > 140 ? COL_BG : COL_TEXT), 0);
}

static const MonNode* findSensor(uint16_t id) {
    const MonNode* nodes = monitor_nodes();
    for (uint8_t i = 0; i < MON_MAX_NODES; i++)
        if (nodes[i].active && nodes[i].id == id) return &nodes[i];
    return nullptr;
}

static uint8_t ropesOn(uint8_t ch) {
    uint8_t n = 0;
    const pulleys::MeshConfigRow* rows = installcfg_rows();
    for (uint8_t i = 0; i < installcfg_row_count(); i++) if (rows[i].channel == ch) n++;
    return n;
}

// ── MAP ROPES ─────────────────────────────────────────────────────────────────
// WAIT    nothing pulled yet
// LISTEN  the window is open: every rope pulled now joins the group
// PICK    window closed; a channel tap moves the whole group. Once moved, the
//         next rope pulled that is not in the group starts a new one.
enum MapState : uint8_t { MAP_WAIT, MAP_LISTEN, MAP_PICK };

static lv_obj_t* s_mapOpen;      // opener
static lv_obj_t* s_map;          // modal
static lv_obj_t* s_mapStatus;
static lv_obj_t* s_mapList;
static lv_obj_t* s_mapBtn[UI_CHANNELS + 1];
static bool      s_mapIsOpen = false;

static MapState  s_state = MAP_WAIT;
static uint16_t  s_cap[MAX_CAPTURE];
static uint8_t   s_capCount = 0;
static uint32_t  s_listenAt = 0;
static int8_t    s_assigned = -1;    // channel the group was moved to, -1 = not yet
static bool      s_restyle  = true;  // repaint the grid on the next refresh

static bool captured(uint16_t id) {
    for (uint8_t i = 0; i < s_capCount; i++) if (s_cap[i] == id) return true;
    return false;
}

static void startGroup(uint16_t id) {
    s_capCount = 0;
    s_cap[s_capCount++] = id;
    s_assigned = -1;
    s_state    = MAP_LISTEN;
    s_listenAt = millis();
}

static void resetGroup() {
    s_capCount = 0;
    s_assigned = -1;
    s_state    = MAP_WAIT;
}

static void refreshMap() {
    char b[400];
    uint32_t now = millis();

    uint8_t confirmed = 0;
    for (uint8_t i = 0; i < s_capCount; i++) {
        const MonNode* n = findSensor(s_cap[i]);
        if (s_assigned >= 0 && n && n->known && n->channel == s_assigned) confirmed++;
    }

    switch (s_state) {
        case MAP_WAIT:
            snprintf(b, sizeof(b), "Pull a rope. Pull several within %d s to put them on one channel.",
                     LISTEN_MS / 1000);
            break;
        case MAP_LISTEN: {
            uint32_t gone = now - s_listenAt;
            uint32_t left = gone < LISTEN_MS ? LISTEN_MS - gone : 0;
            snprintf(b, sizeof(b), "Listening... %lu s   pull any others that share this channel",
                     (unsigned long)((left + 999) / 1000));
            break;
        }
        case MAP_PICK:
            if (s_assigned < 0)
                snprintf(b, sizeof(b), "Tap the channel for %s.",
                         s_capCount == 1 ? "this rope" : "these ropes");
            else if (confirmed < s_capCount)
                snprintf(b, sizeof(b), "Moving to ch %d... %u of %u confirmed",
                         s_assigned, confirmed, s_capCount);
            else
                snprintf(b, sizeof(b), "Done: ch %d. Pull the next rope.", s_assigned);
            break;
    }
    setTextIfChanged(s_mapStatus, b);
    static uint32_t lastCol = 0;
    uint32_t col = s_state == MAP_PICK && s_assigned >= 0 && confirmed == s_capCount ? COL_OK
                 : s_state == MAP_LISTEN ? COL_WARN : COL_TEXT;
    if (col != lastCol) { lv_obj_set_style_text_color(s_mapStatus, lv_color_hex(col), 0); lastCol = col; }

    // One line per rope: where it reports, where the map has it, and whether
    // a move has landed.
    int len = snprintf(b, sizeof(b), "ROPE    NOW  MAP\n");
    for (uint8_t i = 0; i < s_capCount && len < (int)sizeof(b); i++) {
        const MonNode* n = findSensor(s_cap[i]);
        int mapped = installcfg_channel_of(s_cap[i]);
        char nowS[6], mapS[6];
        if (n && n->known) snprintf(nowS, sizeof(nowS), "%2d", n->channel);
        else               snprintf(nowS, sizeof(nowS), " ?");
        if (mapped >= 0)   snprintf(mapS, sizeof(mapS), "%2d", mapped);
        else               snprintf(mapS, sizeof(mapS), " -");
        const char* mark = "";
        if (s_assigned >= 0)
            mark = (n && n->known && n->channel == s_assigned) ? "  ok" : "  ...";
        len += snprintf(b + len, sizeof(b) - len, "N-%04X  %s   %s%s\n",
                        s_cap[i], nowS, mapS, mark);
    }
    setTextIfChanged(s_mapList, b);

    // The grid is live only once there is a closed group to move. Restyled
    // only on a change: every style write repaints, and the panel tears.
    static int8_t lastLive = -1, lastAssigned = -2;
    bool live = s_state == MAP_PICK && s_capCount > 0;
    if (!s_restyle && live == lastLive && s_assigned == lastAssigned) return;
    s_restyle = false;
    lastLive = live; lastAssigned = s_assigned;
    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) {
        lv_obj_set_style_opa(s_mapBtn[ch], live ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_border_width(s_mapBtn[ch], ch == s_assigned ? 4 : 1, 0);
        lv_obj_set_style_border_color(s_mapBtn[ch],
            lv_color_hex(ch == s_assigned ? COL_TEXT : COL_DIV), 0);
    }
}

static void mapChannelCb(lv_event_t* e) {
    uint8_t ch = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (s_state != MAP_PICK || s_capCount == 0) return;
    for (uint8_t i = 0; i < s_capCount; i++) installcfg_set_channel(s_cap[i], ch);
    s_assigned = (int8_t)ch;
    refreshMap();
}

static void mapAgainCb(lv_event_t*) { resetGroup(); refreshMap(); }

static void mapCloseCb(lv_event_t*) {
    s_mapIsOpen = false;
    lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
}

static void mapOpenCb(lv_event_t*) {
    resetGroup();
    s_restyle = true;
    // Colours may have changed since the buttons were painted.
    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) paintChannelButton(s_mapBtn[ch], ch);
    refreshMap();
    s_mapIsOpen = true;
    lv_obj_clear_flag(s_map, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_map);
}

static void buildMap() {
    s_map = makeModal("MAP ROPES");
    s_mapStatus = makeLabel(s_map, 0, 26, &lv_font_montserrat_16, COL_TEXT);

    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) {
        int i = ch - 1;
        char t[4];
        snprintf(t, sizeof(t), "%d", ch);
        s_mapBtn[ch] = makeButton(s_map, (i % GRID_COLS) * 86, 60 + (i / GRID_COLS) * 70,
                                  80, 64, t, COL_BTN, mapChannelCb, (void*)(intptr_t)ch);
        lv_obj_set_style_text_font(lv_obj_get_child(s_mapBtn[ch], 0), &lv_font_montserrat_32, 0);
    }

    s_mapList = makeLabel(s_map, 530, 60, &lv_font_unscii_16, COL_TEXT);
    lv_obj_set_style_text_line_space(s_mapList, 2, 0);

    lv_obj_t* hint = makeLabel(s_map, 0, 346, &lv_font_unscii_8, COL_DIM);
    lv_label_set_text(hint, "pulls go nowhere while this is open: the game does not see them");

    makeButton(s_map, 0,   376, 220, 70, "AGAIN", COL_CANCEL, mapAgainCb, nullptr);
    makeButton(s_map, 556, 376, 220, 70, "DONE",  COL_OK,     mapCloseCb, nullptr);
}

bool installui_on_event(const pulleys::MeshEvent& ev) {
    if (!s_mapIsOpen) return false;
    if (ev.originType != pulleys::MESH_ORIGIN_SENSOR) return true;
    // An ack only says a move landed; the monitor has already recorded where.
    if (!pulleys::mesh_event_is_pull(ev)) { refreshMap(); return true; }

    bool known = captured(ev.originId);
    switch (s_state) {
        case MAP_WAIT:
            startGroup(ev.originId);
            break;
        case MAP_LISTEN:
            if (!known && s_capCount < MAX_CAPTURE) s_cap[s_capCount++] = ev.originId;
            break;
        case MAP_PICK:
            // A new rope after a move is the next one being mapped. Before a
            // move it is someone else pulling; it waits for AGAIN or a move.
            if (!known && s_assigned >= 0) startGroup(ev.originId);
            break;
    }
    refreshMap();
    return true;
}

// ── COLOURS ───────────────────────────────────────────────────────────────────
static lv_obj_t* s_colOpen;
static lv_obj_t* s_col;
static lv_obj_t* s_colBtn[UI_CHANNELS + 1];
static bool      s_colIsOpen = false;

static void refreshColours() {
    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) {
        char t[32];
        uint8_t n = ropesOn(ch);
        snprintf(t, sizeof(t), "%d\n%s  %u", ch,
                 pulleys::color_name(pulleys::channel_color_index(ch)), n);
        setTextIfChanged(lv_obj_get_child(s_colBtn[ch], 0), t);
        paintChannelButton(s_colBtn[ch], ch);
        // A channel with no rope on it can still be coloured, but should not
        // compete for attention with the ones that matter.
        lv_obj_set_style_opa(s_colBtn[ch], n ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

static void colStepCb(lv_event_t* e) {
    uint8_t ch = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    uint8_t next = (uint8_t)((pulleys::channel_color_index(ch) + 1) % pulleys::COLOR_COUNT);
    installcfg_set_color(ch, next);
    refreshColours();
}

static void colShuffleCb(lv_event_t*) { installcfg_shuffle_game1(); refreshColours(); }

static void colCloseCb(lv_event_t*) {
    s_colIsOpen = false;
    lv_obj_add_flag(s_col, LV_OBJ_FLAG_HIDDEN);
}

static void colOpenCb(lv_event_t*) {
    refreshColours();
    s_colIsOpen = true;
    lv_obj_clear_flag(s_col, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_col);
}

static void buildColours() {
    s_col = makeModal("COLOURS");
    lv_obj_t* hint = makeLabel(s_col, 0, 28, &lv_font_unscii_8, COL_DIM);
    lv_label_set_text(hint, "tap a channel to step its colour. the number is how many ropes are on it.");

    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) {
        int i = ch - 1;
        s_colBtn[ch] = makeButton(s_col, (i % GRID_COLS) * 130, 56 + (i / GRID_COLS) * 70,
                                  124, 64, "", COL_BTN, colStepCb, (void*)(intptr_t)ch);
    }

    char legend[96];
    int len = snprintf(legend, sizeof(legend), "game1:");
    for (uint8_t c = 0; c < pulleys::COLOR_GAME1_COUNT; c++)
        len += snprintf(legend + len, sizeof(legend) - len, " %s", pulleys::color_name(c));
    snprintf(legend + len, sizeof(legend) - len, "    game2 finals: %s",
             pulleys::color_name(pulleys::COLOR_GAME2));
    lv_obj_t* l = makeLabel(s_col, 0, 344, &lv_font_unscii_16, COL_TEXT);
    lv_label_set_text(l, legend);

    makeButton(s_col, 0,   376, 220, 70, "SHUFFLE\ngame1 colours", COL_CANCEL, colShuffleCb, nullptr);
    makeButton(s_col, 556, 376, 220, 70, "DONE", COL_OK, colCloseCb, nullptr);
}

// ── Public ────────────────────────────────────────────────────────────────────
void installui_init() {
    // Openers in the right of the title rows of SENSORS (0,0) and CHANNELS
    // (0,1), which the quadrants leave empty.
    s_mapOpen = makeButton(lv_scr_act(), 400 - 124, 3, 118, 26,
                           "MAP ROPES", COL_BTN, mapOpenCb, nullptr);
    s_colOpen = makeButton(lv_scr_act(), 400 - 124, 190 + 3, 118, 26,
                           "COLOURS", COL_BTN, colOpenCb, nullptr);
    buildMap();
    buildColours();
}

void installui_tick() {
    if (!s_mapIsOpen) return;
    uint32_t now = millis();
    if (s_state == MAP_LISTEN && now - s_listenAt >= LISTEN_MS) {
        s_state = MAP_PICK;
        refreshMap();
    }
    // The countdown, and acks landing via the monitor.
    static uint32_t last = 0;
    if (now - last >= 250) { last = now; refreshMap(); }
}

bool installui_is_open() { return s_mapIsOpen || s_colIsOpen; }
