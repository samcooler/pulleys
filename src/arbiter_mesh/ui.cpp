#include "ui.h"
#include "monitor.h"
#include "gamestate.h"
#include "installcfg.h"
#include <lvgl.h>
#include <Arduino.h>
#include <FastLED.h>
#include <pulleys_mesh.h>
#include <pulleys_channel.h>
#include <stdio.h>
#include <string.h>

// ── Four quadrants of an 800×480 panel ────────────────────────────────────────
//
//   ┌───────────────────┬───────────────────┐
//   │ SENSORS           │ SCREENS           │
//   ├───────────────────┼───────────────────┤
//   │ CHANNELS          │ MESH / LOG        │
//   ├───────────────────┴───────────────────┤
//   │ GAME: state, timer, trigger flash+log │
//   └───────────────────────────────────────┘
//
// Everything here is read at a glance from across a dark forest clearing, so
// state is carried by colour as well as by number: green is healthy, amber is
// going quiet, red is lost.

#define COL_BG      0x0d1117
#define COL_PANEL   0x161b22
#define COL_DIV     0x30363d
#define COL_TEXT    0xe6edf3
#define COL_DIM     0x8b949e
#define COL_TITLE   0x58a6ff
#define COL_OK      0x3fb950
#define COL_WARN    0xd29922
#define COL_BAD     0xf85149
#define COL_SENSOR  0x58a6ff
#define COL_SCREEN  0xffa657
#define COL_FLASH   0xf0c000   // a trigger just fired
#define COL_CUE     0x3fb950
#define COL_STATE   0xbc8cff
#define FLASH_MS    1600

#define QW 400
#define QH 190
#define GAME_Y (2 * QH)        // the strip fills the rest of the 480 rows
#define GAME_H (480 - GAME_Y)
#define PAD 10

struct Quadrant {
    lv_obj_t* panel;
    lv_obj_t* title;
    lv_obj_t* body;
};
static Quadrant s_q[4];

// Channel bars live in the bottom-left quadrant: channels 1..UI_CHANNELS (0 is
// the fault channel, and the tail past 24 is headroom no rope is on), three
// columns of eight.
#define UI_CHANNELS  24
#define CHAN_ROWS    8
#define CHAN_COL_W   126
#define CHAN_BAR_X   66
#define CHAN_BAR_MAX 54
static lv_obj_t* s_chanBar[UI_CHANNELS + 1];
static lv_obj_t* s_chanLbl[UI_CHANNELS + 1];

// Game strip
static lv_obj_t* s_gamePanel;
static lv_obj_t* s_gameTitle;
static lv_obj_t* s_gameState;
static lv_obj_t* s_gameTimer;
static lv_obj_t* s_flashBar;
static lv_obj_t* s_flashLbl;
static lv_obj_t* s_gameLog;
static uint32_t  s_seenSeq = 0;
static uint32_t  s_flashAt = 0;
static uint32_t  s_flashCol = COL_FLASH;

static char s_buf[1024];

// The RGB panel streams its framebuffer straight out of PSRAM with no double
// buffer, so every write competes with scanout: repaint less, tear less.
// lv_label_set_text invalidates whether or not the text differs, and most of
// this display is identical from one refresh to the next.
static void setTextIfChanged(lv_obj_t* label, const char* text) {
    const char* cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

static Quadrant makeQuadrant(int col, int row, const char* title) {
    Quadrant q;
    q.panel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(q.panel, QW - 2, QH - 2);
    lv_obj_set_pos(q.panel, col * QW + 1, row * QH + 1);
    lv_obj_set_style_bg_color(q.panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(q.panel, lv_color_hex(COL_DIV), 0);
    lv_obj_set_style_border_width(q.panel, 1, 0);
    lv_obj_set_style_radius(q.panel, 0, 0);
    lv_obj_set_style_pad_all(q.panel, PAD, 0);
    lv_obj_clear_flag(q.panel, LV_OBJ_FLAG_SCROLLABLE);

    q.title = lv_label_create(q.panel);
    lv_label_set_text(q.title, title);
    lv_obj_set_style_text_color(q.title, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(q.title, &lv_font_montserrat_16, 0);
    lv_obj_align(q.title, LV_ALIGN_TOP_LEFT, 0, 0);

    q.body = lv_label_create(q.panel);
    lv_label_set_text(q.body, "");
    lv_obj_set_style_text_color(q.body, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(q.body, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_line_space(q.body, 1, 0);
    lv_obj_align(q.body, LV_ALIGN_TOP_LEFT, 0, 24);
    return q;
}

void ui_init() {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_BG), 0);

    s_q[0] = makeQuadrant(0, 0, "SENSORS");
    s_q[1] = makeQuadrant(1, 0, "SCREENS");
    s_q[2] = makeQuadrant(0, 1, "CHANNELS");
    s_q[3] = makeQuadrant(1, 1, "MESH");

    // Channel rows: a label, then a bar in the channel's colour that matches the
    // physical install (repainted in refreshChannels when the palette changes).
    lv_obj_t* p = s_q[2].panel;
    for (int i = 1; i <= UI_CHANNELS; i++) {
        int col = (i - 1) / CHAN_ROWS, row = (i - 1) % CHAN_ROWS;
        int x = col * CHAN_COL_W;
        int y = 24 + row * 17;

        lv_obj_t* lbl = lv_label_create(p);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x, y + 1);
        s_chanLbl[i] = lbl;

        lv_obj_t* bar = lv_obj_create(p);
        lv_obj_set_size(bar, 2, 10);
        lv_obj_set_pos(bar, x + CHAN_BAR_X, y);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        s_chanBar[i] = bar;
    }

    // Game strip across the bottom: where the state machine is and what it
    // just did. Left, the state and its timer; right, the latest trigger as a
    // bar that flashes and fades, over a short log.
    s_gamePanel = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_gamePanel, 2 * QW - 2, GAME_H - 2);
    lv_obj_set_pos(s_gamePanel, 1, GAME_Y + 1);
    lv_obj_set_style_bg_color(s_gamePanel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(s_gamePanel, lv_color_hex(COL_DIV), 0);
    lv_obj_set_style_border_width(s_gamePanel, 1, 0);
    lv_obj_set_style_radius(s_gamePanel, 0, 0);
    lv_obj_set_style_pad_all(s_gamePanel, 6, 0);
    lv_obj_clear_flag(s_gamePanel, LV_OBJ_FLAG_SCROLLABLE);

    s_gameTitle = lv_label_create(s_gamePanel);
    lv_label_set_text(s_gameTitle, "GAME");
    lv_obj_set_style_text_color(s_gameTitle, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(s_gameTitle, &lv_font_montserrat_16, 0);
    lv_obj_align(s_gameTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_gameState = lv_label_create(s_gamePanel);
    lv_label_set_text(s_gameState, "");
    lv_obj_set_style_text_color(s_gameState, lv_color_hex(COL_STATE), 0);
    lv_obj_set_style_text_font(s_gameState, &lv_font_montserrat_32, 0);
    lv_obj_align(s_gameState, LV_ALIGN_TOP_LEFT, 0, 20);

    s_gameTimer = lv_label_create(s_gamePanel);
    lv_label_set_text(s_gameTimer, "");
    lv_obj_set_style_text_color(s_gameTimer, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(s_gameTimer, &lv_font_unscii_8, 0);
    lv_obj_align(s_gameTimer, LV_ALIGN_TOP_LEFT, 0, 62);

    s_flashBar = lv_obj_create(s_gamePanel);
    lv_obj_set_size(s_flashBar, 2 * QW - 2 - 12 - 270, 26);
    lv_obj_set_pos(s_flashBar, 270, 0);
    lv_obj_set_style_bg_color(s_flashBar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(s_flashBar, 0, 0);
    lv_obj_set_style_radius(s_flashBar, 0, 0);
    lv_obj_set_style_pad_all(s_flashBar, 4, 0);
    lv_obj_clear_flag(s_flashBar, LV_OBJ_FLAG_SCROLLABLE);

    s_flashLbl = lv_label_create(s_flashBar);
    lv_label_set_text(s_flashLbl, "");
    lv_obj_set_style_text_color(s_flashLbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_flashLbl, &lv_font_unscii_16, 0);
    lv_obj_align(s_flashLbl, LV_ALIGN_LEFT_MID, 0, 0);

    s_gameLog = lv_label_create(s_gamePanel);
    lv_label_set_text(s_gameLog, "");
    lv_obj_set_style_text_color(s_gameLog, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(s_gameLog, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_line_space(s_gameLog, 2, 0);
    lv_obj_align(s_gameLog, LV_ALIGN_TOP_LEFT, 270, 32);
}

// Health colour from how long a node has been quiet.
static uint32_t healthColor(uint32_t quietMs) {
    if (quietMs > NODE_LOST_MS)  return COL_BAD;
    if (quietMs > NODE_STALE_MS) return COL_WARN;
    return COL_OK;
}

static const char* ageStr(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    if (s < 100)   snprintf(out, n, "%2lus", (unsigned long)s);
    else if (s < 6000) snprintf(out, n, "%2lum", (unsigned long)(s / 60));
    else           snprintf(out, n, " --");
    return out;
}

// ── Quadrant 0: sensors ───────────────────────────────────────────────────────
static void refreshSensors() {
    const MonNode* nodes = monitor_nodes();
    uint32_t now = millis();
    int len = 0;
    len += snprintf(s_buf + len, sizeof(s_buf) - len,
                    "ID    CH  MODE EVENTS LAST SEEN\n");

    int shown = 0;
    for (uint8_t i = 0; i < MON_MAX_NODES && shown < 7; i++) {
        const MonNode& n = nodes[i];
        if (!n.active || n.type != pulleys::MESH_ORIGIN_SENSOR) continue;
        char lastS[8], seenS[8];
        uint32_t quiet = now - (n.lastBeaconMs > n.lastEventMs ? n.lastBeaconMs : n.lastEventMs);
        ageStr(n.lastEventMs ? now - n.lastEventMs : 0xFFFFFFFF, lastS, sizeof(lastS));
        ageStr(quiet, seenS, sizeof(seenS));
        // Channel and mode ride on EVENT packets only. A sensor that has
        // beaconed but not yet fired is present with nothing known about it,
        // and "ch0 rot" would be a fabrication. A '!' after the channel is a
        // rope not (yet) where the arbiter's map puts it.
        char chS[5], modeS[4];
        if (n.known) {
            int mapped = installcfg_channel_of(n.id);
            snprintf(chS, sizeof(chS), "%2d%c", n.channel,
                     mapped >= 0 && mapped != n.channel ? '!' : ' ');
            snprintf(modeS, sizeof(modeS), "%s",
                     n.mode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin");
        } else {
            snprintf(chS, sizeof(chS), " ? ");
            snprintf(modeS, sizeof(modeS), " ? ");
        }
        len += snprintf(s_buf + len, sizeof(s_buf) - len,
                        "%04X  %s %s %6lu  %s %s\n",
                        n.id, chS, modeS,
                        (unsigned long)n.events, lastS, seenS);
        shown++;
    }
    if (shown == 0)
        len += snprintf(s_buf + len, sizeof(s_buf) - len, "\n  no sensors heard yet");

    setTextIfChanged(s_q[0].body, s_buf);

    char t[48];
    snprintf(t, sizeof(t), "SENSORS  %d", monitor_count(pulleys::MESH_ORIGIN_SENSOR));
    setTextIfChanged(s_q[0].title, t);
    lv_obj_set_style_text_color(s_q[0].title, lv_color_hex(COL_SENSOR), 0);
}

// ── Quadrant 1: screens ───────────────────────────────────────────────────────
static void refreshScreens() {
    const MonNode* nodes = monitor_nodes();
    uint32_t now = millis();
    int len = 0;
    len += snprintf(s_buf + len, sizeof(s_buf) - len,
                    "ID    SKEW    LAST BEACON  UP\n");

    int shown = 0;
    for (uint8_t i = 0; i < MON_MAX_NODES && shown < 5; i++) {
        const MonNode& n = nodes[i];
        if (!n.active || n.type != pulleys::MESH_ORIGIN_SCREEN) continue;
        char seenS[8], upS[8];
        uint32_t quiet = now - n.lastBeaconMs;
        ageStr(quiet, seenS, sizeof(seenS));
        ageStr(now - n.firstSeenMs, upS, sizeof(upS));
        len += snprintf(s_buf + len, sizeof(s_buf) - len,
                        "%04X %+5ldms      %s  %s\n",
                        n.id, (long)n.skewMs, seenS, upS);
        shown++;
    }
    if (shown == 0)
        len += snprintf(s_buf + len, sizeof(s_buf) - len, "\n  no screens heard yet");

    // A screen emits no events, so its beacon is the only proof it is alive.
    len += snprintf(s_buf + len, sizeof(s_buf) - len,
                    "\nscreens beacon every %lus",
                    (unsigned long)(pulleys::MESH_SYNC_INTERVAL_MS / 1000));

    setTextIfChanged(s_q[1].body, s_buf);

    char t[48];
    snprintf(t, sizeof(t), "SCREENS  %d", monitor_count(pulleys::MESH_ORIGIN_SCREEN));
    setTextIfChanged(s_q[1].title, t);
    lv_obj_set_style_text_color(s_q[1].title, lv_color_hex(COL_SCREEN), 0);
}

// ── Quadrant 2: channels ──────────────────────────────────────────────────────
static void refreshChannels() {
    const MonChannel* ch = monitor_channels();

    float peak = 0.5f;
    for (int i = 1; i <= UI_CHANNELS; i++) if (ch[i].activity > peak) peak = ch[i].activity;

    for (int i = 1; i <= UI_CHANNELS; i++) {
        char l[24];
        snprintf(l, sizeof(l), "%2d %c%4lu", i,
                 ch[i].sensors ? '*' : ' ', (unsigned long)ch[i].events);
        setTextIfChanged(s_chanLbl[i], l);

        // The palette is the arbiter's to change at any moment.
        static int lastColor[UI_CHANNELS + 1] = {};   // colour + 1; 0 = not painted yet
        int color = pulleys::channel_color_index(i) + 1;
        if (color != lastColor[i]) {
            CRGB c = pulleys::channel_color(i);
            lv_obj_set_style_bg_color(s_chanBar[i], lv_color_make(c.r, c.g, c.b), 0);
            lastColor[i] = color;
        }

        int w = (int)((ch[i].activity / peak) * (float)CHAN_BAR_MAX);
        if (w < 2) w = 2;
        static int  lastW[UI_CHANNELS + 1] = {};
        static bool lastLit[UI_CHANNELS + 1] = {};
        bool lit = ch[i].activity > 0.01f;
        if (w != lastW[i]) { lv_obj_set_width(s_chanBar[i], w); lastW[i] = w; }
        // Idle channels dim right down so the busy ones carry the eye.
        if (lit != lastLit[i]) {
            lv_obj_set_style_opa(s_chanBar[i], lit ? LV_OPA_COVER : LV_OPA_30, 0);
            lastLit[i] = lit;
        }
    }
}

// ── Quadrant 3: mesh health + rolling event log ───────────────────────────────
static void refreshMesh() {
    const MonLogEntry* log = monitor_log();
    uint32_t now = millis();
    int len = 0;

    uint32_t total = monitor_total_events();
    uint32_t relay = monitor_relayed_events();
    int32_t  spread = monitor_skew_spread();

    len += snprintf(s_buf + len, sizeof(s_buf) - len,
                    "events %lu  (%.0f/min)\n"
                    "relayed %lu (%d%%)  skew %+ldms\n"
                    "clock %s   uptime %lus\n"
                    "--- recent -----------------\n",
                    (unsigned long)total, monitor_events_per_min(),
                    (unsigned long)relay,
                    total ? (int)(relay * 100 / total) : 0,
                    (long)spread,
                    pulleys::mesh_clock_locked() ? "LOCKED" : "free",
                    (unsigned long)(now / 1000));

    uint8_t n = monitor_log_count();
    if (n == 0)
        len += snprintf(s_buf + len, sizeof(s_buf) - len, "  nothing yet");
    for (uint8_t i = 0; i < n && i < 8; i++) {
        const MonLogEntry& e = log[i];
        char age[8];
        ageStr(now - e.atMs, age, sizeof(age));
        len += snprintf(s_buf + len, sizeof(s_buf) - len,
                        "%s %04X ch%-2d mag%3d ttl%d%s\n",
                        age, e.originId, e.channel, e.magnitude, e.ttl,
                        e.relayed ? " R" : "");
    }

    setTextIfChanged(s_q[3].body, s_buf);
    lv_obj_set_style_text_font(s_q[3].body, &lv_font_unscii_8, 0);
}

// ── Game strip ────────────────────────────────────────────────────────────────
static uint32_t kindColor(char kind) {
    switch (kind) {
        case 'C': return COL_CUE;      // a cue went out
        case 'S': return COL_STATE;    // the state changed
        case 'T': return COL_WARN;     // a timer ran out
        default:  return COL_FLASH;    // an event the state reacted to
    }
}

static void fmtClock(int32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void refreshGame() {
    char t[64];
    snprintf(t, sizeof(t), "GAME  %s", gamestate_ready() ? gamestate_game() : "(none loaded)");
    setTextIfChanged(s_gameTitle, t);
    setTextIfChanged(s_gameState, gamestate_ready() ? gamestate_state() : "-");

    if (gamestate_ready()) {
        char age[12], left[12];
        fmtClock(gamestate_state_age_ms(), age, sizeof(age));
        int32_t remain = gamestate_timer_left_ms();
        if (remain >= 0) {
            fmtClock(remain + 999, left, sizeof(left));   // round up, so 0:00 means it fired
            snprintf(t, sizeof(t), "in state %s\nthen %s in %s", age,
                     gamestate_timer_target(), left);
        } else {
            snprintf(t, sizeof(t), "in state %s\nno timer", age);
        }
        setTextIfChanged(s_gameTimer, t);
    } else {
        setTextIfChanged(s_gameTimer, "");
    }

    // Log, newest first; the top line is what the flash bar is showing.
    const GameTrigger* log = gamestate_log();
    uint8_t n = gamestate_log_count();
    int len = 0;
    s_buf[0] = 0;
    for (uint8_t i = 1; i < n && i < 5; i++) {
        char age[12];
        fmtClock((int32_t)(millis() - log[i].atMs), age, sizeof(age));
        len += snprintf(s_buf + len, sizeof(s_buf) - len, "%s%s  %c %s", i > 1 ? "\n" : "",
                        age, log[i].kind, log[i].text);
    }
    setTextIfChanged(s_gameLog, s_buf);
}

// A trigger lights the bar at full strength and lets it fade back to the
// panel. Called every loop: it does nothing until something fires, and then
// only while the fade runs.
void ui_tick() {
    uint32_t seq = gamestate_trigger_seq();
    uint32_t now = millis();

    if (seq != s_seenSeq) {
        s_seenSeq = seq;
        s_flashAt = now ? now : 1;
        const GameTrigger& g = gamestate_log()[0];
        s_flashCol = kindColor(g.kind);
        char line[64];
        snprintf(line, sizeof(line), "%c %s", g.kind, g.text);
        setTextIfChanged(s_flashLbl, line);
        refreshGame();
    }

    static uint32_t lastStep = 0;
    if (!s_flashAt || now - lastStep < 50) return;
    lastStep = now;

    uint32_t age = now - s_flashAt;
    float k = age >= FLASH_MS ? 0.0f : 1.0f - (float)age / FLASH_MS;
    lv_color_t from = lv_color_hex(COL_PANEL), to = lv_color_hex(s_flashCol);
    lv_obj_set_style_bg_color(s_flashBar, lv_color_mix(to, from, (uint8_t)(k * 255)), 0);
    // Dark text on the bright bar, light once it has faded.
    lv_obj_set_style_text_color(s_flashLbl,
        lv_color_hex(k > 0.5f ? COL_BG : COL_TEXT), 0);
    if (age >= FLASH_MS) s_flashAt = 0;
}

void ui_refresh() {
    refreshGame();
    refreshSensors();
    refreshScreens();
    refreshChannels();
    refreshMesh();
}
