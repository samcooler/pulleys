#include "ui.h"
#include "monitor.h"
#include <lvgl.h>
#include <Arduino.h>
#include <FastLED.h>
#include <pulleys_mesh.h>
#include <pulleys_channel.h>
#include <stdio.h>

// ── Four quadrants of an 800×480 panel ────────────────────────────────────────
//
//   ┌───────────────────┬───────────────────┐
//   │ SENSORS           │ SCREENS           │
//   ├───────────────────┼───────────────────┤
//   │ CHANNELS          │ MESH / LOG        │
//   └───────────────────┴───────────────────┘
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

#define QW 400
#define QH 240
#define PAD 10

struct Quadrant {
    lv_obj_t* panel;
    lv_obj_t* title;
    lv_obj_t* body;
};
static Quadrant s_q[4];

// Channel bars live in the bottom-left quadrant
static lv_obj_t* s_chanBar[MON_CHANNELS];
static lv_obj_t* s_chanLbl[MON_CHANNELS];

static char s_buf[1024];

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

    // Channel rows: a hue swatch that matches the physical install, then a bar.
    // Two columns of eight so all 16 channels fit the quadrant.
    lv_obj_t* p = s_q[2].panel;
    for (int i = 0; i < MON_CHANNELS; i++) {
        int col = i / 8, row = i % 8;
        int x = col * 190;
        int y = 24 + row * 22;

        lv_obj_t* lbl = lv_label_create(p);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x, y + 3);
        s_chanLbl[i] = lbl;

        lv_obj_t* bar = lv_obj_create(p);
        lv_obj_set_size(bar, 2, 12);
        lv_obj_set_pos(bar, x + 58, y);
        CRGB c = pulleys::channel_color(i);
        lv_obj_set_style_bg_color(bar, lv_color_make(c.r, c.g, c.b), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        s_chanBar[i] = bar;
    }
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
                    "ID    CH MODE EVENTS LAST SEEN\n");

    int shown = 0;
    for (uint8_t i = 0; i < MON_MAX_NODES && shown < 10; i++) {
        const MonNode& n = nodes[i];
        if (!n.active || n.type != pulleys::MESH_ORIGIN_SENSOR) continue;
        char lastS[8], seenS[8];
        uint32_t quiet = now - (n.lastBeaconMs > n.lastEventMs ? n.lastBeaconMs : n.lastEventMs);
        ageStr(n.lastEventMs ? now - n.lastEventMs : 0xFFFFFFFF, lastS, sizeof(lastS));
        ageStr(quiet, seenS, sizeof(seenS));
        len += snprintf(s_buf + len, sizeof(s_buf) - len,
                        "%04X  %2d %s %6lu  %s %s\n",
                        n.id, n.channel,
                        n.mode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin",
                        (unsigned long)n.events, lastS, seenS);
        shown++;
    }
    if (shown == 0)
        len += snprintf(s_buf + len, sizeof(s_buf) - len, "\n  no sensors heard yet");

    lv_label_set_text(s_q[0].body, s_buf);

    char t[48];
    snprintf(t, sizeof(t), "SENSORS  %d", monitor_count(pulleys::MESH_ORIGIN_SENSOR));
    lv_label_set_text(s_q[0].title, t);
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
    for (uint8_t i = 0; i < MON_MAX_NODES && shown < 10; i++) {
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
                    "\nbeacon every %lus; a screen is\nsilent otherwise.",
                    (unsigned long)(pulleys::MESH_SYNC_INTERVAL_MS / 1000));

    lv_label_set_text(s_q[1].body, s_buf);

    char t[48];
    snprintf(t, sizeof(t), "SCREENS  %d", monitor_count(pulleys::MESH_ORIGIN_SCREEN));
    lv_label_set_text(s_q[1].title, t);
    lv_obj_set_style_text_color(s_q[1].title, lv_color_hex(COL_SCREEN), 0);
}

// ── Quadrant 2: channels ──────────────────────────────────────────────────────
static void refreshChannels() {
    const MonChannel* ch = monitor_channels();

    float peak = 0.5f;
    for (int i = 0; i < MON_CHANNELS; i++) if (ch[i].activity > peak) peak = ch[i].activity;

    for (int i = 0; i < MON_CHANNELS; i++) {
        char l[24];
        snprintf(l, sizeof(l), "%2d %c%4lu", i,
                 ch[i].sensors ? '*' : ' ', (unsigned long)ch[i].events);
        lv_label_set_text(s_chanLbl[i], l);

        int w = (int)((ch[i].activity / peak) * 120.0f);
        if (w < 2) w = 2;
        lv_obj_set_width(s_chanBar[i], w);
        // Idle channels dim right down so the busy ones carry the eye.
        lv_obj_set_style_opa(s_chanBar[i],
                             ch[i].activity > 0.01f ? LV_OPA_COVER : LV_OPA_30, 0);
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
    for (uint8_t i = 0; i < n && i < 9; i++) {
        const MonLogEntry& e = log[i];
        char age[8];
        ageStr(now - e.atMs, age, sizeof(age));
        len += snprintf(s_buf + len, sizeof(s_buf) - len,
                        "%s %04X ch%-2d mag%3d ttl%d%s\n",
                        age, e.originId, e.channel, e.magnitude, e.ttl,
                        e.relayed ? " R" : "");
    }

    lv_label_set_text(s_q[3].body, s_buf);
    lv_obj_set_style_text_font(s_q[3].body, &lv_font_unscii_8, 0);
}

void ui_refresh() {
    refreshSensors();
    refreshScreens();
    refreshChannels();
    refreshMesh();
}
