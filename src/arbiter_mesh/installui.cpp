#include "installui.h"
#include "installcfg.h"
#include "installmap.h"
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

#define LISTEN_MS    MappingSession::LISTEN_MS

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
static lv_obj_t* s_mapOpen;
static lv_obj_t* s_map;
static lv_obj_t* s_mapStatus;
static lv_obj_t* s_mapList;
static lv_obj_t* s_mapBtn[UI_CHANNELS + 1];
static bool s_mapIsOpen = false;
static bool s_restyle = true;

static void refreshMap() {
    const auto& session = installmap_state();
    auto phase = session.phase;
    auto count = session.count;
    const auto* ids = session.ids;
    auto assigned = session.assigned;
    auto listenAt = session.listenAt;
    char b[400];
    uint32_t now = millis();

    uint8_t confirmed = 0;
    for (uint8_t i = 0; i < count; i++) {
        const MonNode* n = findSensor(ids[i]);
        if (assigned >= 0 && n && n->known && n->channel == assigned) confirmed++;
    }

    switch (phase) {
        case MappingSession::WAIT:
            snprintf(b, sizeof(b), "Pull a rope. Pull several within %d s to put them on one channel.",
                     LISTEN_MS / 1000);
            break;
        case MappingSession::LISTEN: {
            uint32_t gone = now - listenAt;
            uint32_t left = gone < LISTEN_MS ? LISTEN_MS - gone : 0;
            snprintf(b, sizeof(b), "Listening... %lu s   pull any others that share this channel",
                     (unsigned long)((left + 999) / 1000));
            break;
        }
        case MappingSession::PICK:
            if (assigned < 0)
                snprintf(b, sizeof(b), "Tap the channel for %s.",
                         count == 1 ? "this rope" : "these ropes");
            else if (confirmed < count)
                snprintf(b, sizeof(b), "Moving to ch %d... %u of %u confirmed",
                         assigned, confirmed, count);
            else
                snprintf(b, sizeof(b), "Done: ch %d. Pull the next rope.", assigned);
            break;
    }
    setTextIfChanged(s_mapStatus, b);
    static uint32_t lastCol = 0;
    uint32_t col = phase == MappingSession::PICK && assigned >= 0 && confirmed == count ? COL_OK
                 : phase == MappingSession::LISTEN ? COL_WARN : COL_TEXT;
    if (col != lastCol) { lv_obj_set_style_text_color(s_mapStatus, lv_color_hex(col), 0); lastCol = col; }

    // One line per rope: where it reports, where the map has it, and whether
    // a move has landed.
    int len = snprintf(b, sizeof(b), "ROPE    NOW  MAP\n");
    for (uint8_t i = 0; i < count && len < (int)sizeof(b); i++) {
        const MonNode* n = findSensor(ids[i]);
        int mapped = installcfg_channel_of(ids[i]);
        char nowS[6], mapS[6];
        if (n && n->known) snprintf(nowS, sizeof(nowS), "%2d", n->channel);
        else               snprintf(nowS, sizeof(nowS), " ?");
        if (mapped >= 0)   snprintf(mapS, sizeof(mapS), "%2d", mapped);
        else               snprintf(mapS, sizeof(mapS), " -");
        const char* mark = "";
        if (assigned >= 0)
            mark = (n && n->known && n->channel == assigned) ? "  ok" : "  ...";
        len += snprintf(b + len, sizeof(b) - len, "N-%04X  %s   %s%s\n",
                        ids[i], nowS, mapS, mark);
    }
    setTextIfChanged(s_mapList, b);

    // The grid is live only once there is a closed group to move. Restyled
    // only on a change: every style write repaints, and the panel tears.
    static int8_t lastLive = -1, lastAssigned = -2;
    bool live = phase == MappingSession::PICK && count > 0;
    if (!s_restyle && live == lastLive && assigned == lastAssigned) return;
    s_restyle = false;
    lastLive = live; lastAssigned = assigned;
    for (uint8_t ch = 1; ch <= UI_CHANNELS; ch++) {
        lv_obj_set_style_opa(s_mapBtn[ch], live ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_border_width(s_mapBtn[ch], ch == assigned ? 4 : 1, 0);
        lv_obj_set_style_border_color(s_mapBtn[ch],
            lv_color_hex(ch == assigned ? COL_TEXT : COL_DIV), 0);
    }
}

static void mapChannelCb(lv_event_t* e) {
    uint8_t ch = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    installmap_assign(ch);
    refreshMap();
}

static void mapAgainCb(lv_event_t*) { installmap_reset(); refreshMap(); }

static void mapCloseCb(lv_event_t*) {
    installmap_close(0, 0);
    s_mapIsOpen = false;
    lv_obj_add_flag(s_map, LV_OBJ_FLAG_HIDDEN);
}

static void mapOpenCb(lv_event_t*) {
    if (!installmap_open(0, 0)) return;
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
    bool consumed = installmap_on_event(ev);
    if (consumed && s_mapIsOpen) refreshMap();
    return consumed;
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

static void tuneOpenCb(lv_event_t*);   // TUNE RGB, below

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
    makeButton(s_col, 278, 376, 220, 70, "TUNE RGB", COL_BTN, tuneOpenCb, nullptr);
    makeButton(s_col, 556, 376, 220, 70, "DONE", COL_OK, colCloseCb, nullptr);
}

// ── TUNE RGB ──────────────────────────────────────────────────────────────────
// What each of the six colours actually is. One row per colour: a swatch, then
// a -/+ stepper for each of R, G and B. A tap steps by RGB_STEP; holding a
// button keeps stepping. Every rope and screen wearing the colour follows a
// moment after the stepping stops (installcfg settles before it sends).
//
// Worth knowing while tuning: the LEDs mix differently from this LCD, so judge
// on a rope, not on the swatch -- and a whiter colour lights all three LED dies,
// which is more heat in a potted panel than any saturated one.
#define RGB_STEP 8

static lv_obj_t* s_tune;
static lv_obj_t* s_tuneSwatch[pulleys::COLOR_COUNT];
static lv_obj_t* s_tuneVal[pulleys::COLOR_COUNT][3];
static bool      s_tuneIsOpen = false;

static void refreshTune() {
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; c++) {
        CRGB v = pulleys::color_rgb(c);
        lv_obj_set_style_bg_color(s_tuneSwatch[c], lv_color_make(v.r, v.g, v.b), 0);
        const uint8_t parts[3] = { v.r, v.g, v.b };
        for (uint8_t k = 0; k < 3; k++) {
            char t[4];
            snprintf(t, sizeof(t), "%u", parts[k]);
            setTextIfChanged(s_tuneVal[c][k], t);
        }
    }
}

// user data: colour << 3 | component << 1 | up
static void tuneStepCb(lv_event_t* e) {
    intptr_t code = (intptr_t)lv_event_get_user_data(e);
    uint8_t c = (uint8_t)(code >> 3), k = (uint8_t)((code >> 1) & 3);
    int dir = (code & 1) ? RGB_STEP : -RGB_STEP;
    CRGB v = pulleys::color_rgb(c);
    uint8_t* part = k == 0 ? &v.r : k == 1 ? &v.g : &v.b;
    *part = (uint8_t)constrain((int)*part + dir, 0, 255);
    installcfg_set_rgb(c, v.r, v.g, v.b);
    refreshTune();
}

static void tuneResetCb(lv_event_t*) { installcfg_reset_rgb(); refreshTune(); }

static void tuneCloseCb(lv_event_t*) {
    s_tuneIsOpen = false;
    lv_obj_add_flag(s_tune, LV_OBJ_FLAG_HIDDEN);
    refreshColours();   // back on COLOURS, whose buttons wear these colours
}

static void tuneOpenCb(lv_event_t*) {
    refreshTune();
    s_tuneIsOpen = true;
    lv_obj_clear_flag(s_tune, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_tune);
}

static void buildTune() {
    s_tune = makeModal("TUNE RGB");
    lv_obj_t* hint = makeLabel(s_tune, 140, 4, &lv_font_unscii_8, COL_DIM);
    lv_label_set_text(hint, "judge on a rope, not this screen. whiter = hotter panels.");

    static const char* const PART[3] = { "R", "G", "B" };
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; c++) {
        int y = 30 + c * 56;
        s_tuneSwatch[c] = lv_obj_create(s_tune);
        lv_obj_set_size(s_tuneSwatch[c], 44, 46);
        lv_obj_set_pos(s_tuneSwatch[c], 0, y);
        lv_obj_set_style_radius(s_tuneSwatch[c], 4, 0);
        lv_obj_set_style_border_width(s_tuneSwatch[c], 1, 0);
        lv_obj_set_style_border_color(s_tuneSwatch[c], lv_color_hex(COL_DIV), 0);
        lv_obj_clear_flag(s_tuneSwatch[c], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name = makeLabel(s_tune, 52, y + 14, &lv_font_unscii_16, COL_TEXT);
        lv_label_set_text(name, pulleys::color_name(c));

        for (uint8_t k = 0; k < 3; k++) {
            int x = 160 + k * 204;
            lv_obj_t* tag = makeLabel(s_tune, x, y + 14, &lv_font_unscii_16, COL_DIM);
            lv_label_set_text(tag, PART[k]);
            intptr_t code = (intptr_t)((c << 3) | (k << 1));
            lv_obj_t* down = makeButton(s_tune, x + 20, y, 56, 46, LV_SYMBOL_MINUS, COL_BTN,
                                        tuneStepCb, (void*)code);
            lv_obj_add_event_cb(down, tuneStepCb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)code);
            s_tuneVal[c][k] = makeLabel(s_tune, x + 86, y + 14, &lv_font_unscii_16, COL_TEXT);
            lv_obj_t* up = makeButton(s_tune, x + 128, y, 56, 46, LV_SYMBOL_PLUS, COL_BTN,
                                      tuneStepCb, (void*)(code | 1));
            lv_obj_add_event_cb(up, tuneStepCb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(code | 1));
        }
    }

    makeButton(s_tune, 0,   376, 220, 70, "RESET\nall to default", COL_CANCEL, tuneResetCb, nullptr);
    makeButton(s_tune, 556, 376, 220, 70, "DONE", COL_OK, tuneCloseCb, nullptr);
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
    buildTune();
}

void installui_tick() {
    installmap_tick();
    static uint32_t lastRevision = 0;
    if (lastRevision != installcfg_revision()) {
        lastRevision = installcfg_revision();
        if (s_colIsOpen) refreshColours();
        if (s_tuneIsOpen) refreshTune();
        if (s_mapIsOpen)
            for (uint8_t ch = 1; ch <= UI_CHANNELS; ++ch) paintChannelButton(s_mapBtn[ch], ch);
    }
    // Give touch operators a visible reason when a browser owns mapping.
    setTextIfChanged(lv_obj_get_child(s_mapOpen, 0),
        installmap_state().active && installmap_state().owner ? "MAP: REMOTE" : "MAP ROPES");
    if (!s_mapIsOpen) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last >= 250) { last = now; refreshMap(); }
}

bool installui_is_open() { return s_mapIsOpen || s_colIsOpen || s_tuneIsOpen; }
