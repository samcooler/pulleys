#include "arbiter_ui.h"
#include "../ble_scanner.h"
#include <FastLED.h>
#include <pulleys_patterns.h>
#include <pulleys_protocol.h>
#include <Arduino.h>
#include <stdio.h>

// ── Palette ───────────────────────────────────────────────────────────────────
#define COL_BG       0x0d1117
#define COL_PANEL_L  0x161b22
#define COL_ROW_NORM 0x1c2333
#define COL_ROW_SEL  0x1f4068
#define COL_TEXT     0xe6edf3
#define COL_DIM      0x8b949e
#define COL_TRAVELER 0x58a6ff
#define COL_STATION  0xffa657
#define COL_DIV      0x30363d
#define COL_CHART_BG 0x010409
#define COL_GRID_LN  0x21262d

#define LIST_W   280
#define DETAIL_X (LIST_W + 1)
#define DETAIL_W (800 - LIST_W - 1)   // 519px
#define PANEL_H  480
#define PAD      24

// Right-panel content area = 519 - 2*24 = 471px wide, 480 - 2*24 = 432px tall
// Two-column split below the info area (y≥80):
//   Left col  [x=0,   w=258]: signal chart
//   Right col [x=271, w=200]: 8×8 LED pattern preview
#define COL_L_W  258
#define COL_R_X  271
#define COL_R_W  200

// 8×8 grid: 23px cells, 2px gap → step=25, total=7*25+23=198px ≤ COL_R_W
#define CELL_SZ  23
#define CELL_STP 25

// ── Widget handles ────────────────────────────────────────────────────────────
static lv_obj_t*          s_list_cont  = nullptr;

static lv_obj_t*          s_det_title  = nullptr;
static lv_obj_t*          s_det_mac    = nullptr;
static lv_obj_t*          s_det_rssi   = nullptr;
static lv_obj_t*          s_det_seen   = nullptr;
static lv_obj_t*          s_det_chart  = nullptr;
static lv_chart_series_t* s_det_series = nullptr;
static lv_obj_t*          s_det_shape  = nullptr;
static lv_obj_t*          s_grid_cells[64];

static lv_coord_t s_chart_pts[RSSI_HISTORY_LEN];

static int      s_selected_idx     = -1;
static bool     s_needs_refresh    = false;
static uint16_t s_last_grid_id     = 0xFFFF;

// ── Pattern animation ─────────────────────────────────────────────────────────
static CRGB                s_led_buf[64];
static pulleys::PatternSlot s_anim_slot;
static bool                s_anim_active  = false;
static uint32_t            s_anim_last_ms = 0;

static void anim_start(const ArbiterDevice* d) {
    if (d->id == s_last_grid_id) return;
    s_last_grid_id = d->id;

    // Seed with hardware RNG XOR'd with device ID. FastLED's PRNG is a shared
    // global touched by NimBLE tasks, so determinism across boots isn't possible.
    random16_set_seed((uint16_t)(esp_random() ^ d->id));

    s_anim_slot.buffer  = s_led_buf;
    s_anim_slot.rows    = 8;
    s_anim_slot.cols    = 8;
    s_anim_slot.maxBri  = 255;
    s_anim_slot.culture = d->culture;
    s_anim_slot.init(pulleys::PATTERN_SHAPE, 8, 8);

    s_anim_last_ms = millis();
    s_anim_active  = true;
}

static void anim_stop() {
    s_anim_active  = false;
    s_last_grid_id = 0xFFFF;
    for (int i = 0; i < 64; i++)
        lv_obj_set_style_bg_color(s_grid_cells[i], lv_color_hex(0x111111), LV_PART_MAIN);
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static lv_color_t rssi_color(int8_t rssi) {
    if (rssi >= -60) return lv_color_hex(0x3fb950);
    if (rssi >= -75) return lv_color_hex(0xd29922);
    return lv_color_hex(0xf85149);
}

// ── Detail panel ─────────────────────────────────────────────────────────────
static void detail_clear() {
    lv_label_set_text(s_det_title, "Tap a device");
    lv_obj_set_style_text_color(s_det_title, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_label_set_text(s_det_mac,   "");
    lv_label_set_text(s_det_rssi,  "");
    lv_label_set_text(s_det_seen,  "");
    lv_label_set_text(s_det_shape, "");
    anim_stop();
    lv_chart_set_point_count(s_det_chart, 1);
    s_chart_pts[0] = LV_CHART_POINT_NONE;
    lv_chart_set_ext_y_array(s_det_chart, s_det_series, s_chart_pts);
    lv_chart_refresh(s_det_chart);
}

static void detail_show(const ArbiterDevice* d) {
    char buf[80];

    snprintf(buf, sizeof(buf), "%s  %04X",
             d->type == PULLEYS_TYPE_TRAVELER ? "TRAVELER" : "STATION", d->id);
    lv_label_set_text(s_det_title, buf);
    lv_obj_set_style_text_color(s_det_title,
        lv_color_hex(d->type == PULLEYS_TYPE_TRAVELER ? COL_TRAVELER : COL_STATION),
        LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             d->mac[5], d->mac[4], d->mac[3], d->mac[2], d->mac[1], d->mac[0]);
    lv_label_set_text(s_det_mac, buf);

    snprintf(buf, sizeof(buf), "%d dBm", d->current_rssi);
    lv_label_set_text(s_det_rssi, buf);
    lv_obj_set_style_text_color(s_det_rssi, rssi_color(d->current_rssi), LV_PART_MAIN);

    uint32_t ago = millis() - d->last_seen_ms;
    snprintf(buf, sizeof(buf), "%.1f s ago", ago / 1000.0f);
    lv_label_set_text(s_det_seen, buf);

    // RSSI chart
    uint8_t n = d->rssi_count;
    if (n > 0) {
        uint8_t start = (n == RSSI_HISTORY_LEN) ? d->rssi_head : 0;
        for (uint8_t i = 0; i < n; i++) {
            uint8_t idx = (start + i) % RSSI_HISTORY_LEN;
            int8_t  v   = d->rssi_buf[idx];
            s_chart_pts[i] = (v == -127) ? LV_CHART_POINT_NONE : (lv_coord_t)v;
        }
        lv_chart_set_point_count(s_det_chart, n);
        lv_chart_set_ext_y_array(s_det_chart, s_det_series, s_chart_pts);
        lv_chart_refresh(s_det_chart);
    }

    anim_start(d);

    snprintf(buf, sizeof(buf), "%s  osc %u",
             pulleys::shape_name(d->culture.shape), d->culture.oscillation);
    lv_label_set_text(s_det_shape, buf);
}

// ── List rebuild ─────────────────────────────────────────────────────────────
static void row_click_cb(lv_event_t* e) {
    s_selected_idx  = (int)(intptr_t)lv_event_get_user_data(e);
    s_needs_refresh = true;
}

static void rebuild_list() {
    lv_obj_clean(s_list_cont);

    if (scanner_count() == 0) {
        lv_obj_t* lbl = lv_label_create(s_list_cont);
        lv_label_set_text(lbl, "Scanning...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM), LV_PART_MAIN);
        return;
    }

    const ArbiterDevice* devs = scanner_devices();
    for (int i = 0; i < ARBITER_MAX_DEVICES; i++) {
        const ArbiterDevice& d = devs[i];
        if (!d.active) continue;

        bool sel = (i == s_selected_idx);

        lv_obj_t* row = lv_obj_create(s_list_cont);
        lv_obj_set_size(row, LV_PCT(100), 54);
        lv_obj_set_style_bg_color(row,
            lv_color_hex(sel ? COL_ROW_SEL : COL_ROW_NORM), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* sw = lv_obj_create(row);
        lv_obj_set_size(sw, 6, 36);
        lv_obj_set_style_bg_color(sw,
            lv_color_make(d.culture.colorA.r, d.culture.colorA.g, d.culture.colorA.b),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(sw, 2, LV_PART_MAIN);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* type_lbl = lv_label_create(row);
        lv_label_set_text(type_lbl,
            d.type == PULLEYS_TYPE_TRAVELER ? "T" : "S");
        lv_obj_set_style_text_color(type_lbl,
            lv_color_hex(d.type == PULLEYS_TYPE_TRAVELER ? COL_TRAVELER : COL_STATION),
            LV_PART_MAIN);
        lv_obj_align(type_lbl, LV_ALIGN_LEFT_MID, 14, -9);

        char id_s[8];
        snprintf(id_s, sizeof(id_s), "%04X", d.id);
        lv_obj_t* id_lbl = lv_label_create(row);
        lv_label_set_text(id_lbl, id_s);
        lv_obj_set_style_text_color(id_lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
        lv_obj_align(id_lbl, LV_ALIGN_LEFT_MID, 26, -9);

        char rssi_s[10];
        snprintf(rssi_s, sizeof(rssi_s), "%d dBm", d.current_rssi);
        lv_obj_t* rssi_lbl = lv_label_create(row);
        lv_label_set_text(rssi_lbl, rssi_s);
        lv_obj_set_style_text_color(rssi_lbl, rssi_color(d.current_rssi), LV_PART_MAIN);
        lv_obj_align(rssi_lbl, LV_ALIGN_LEFT_MID, 14, 9);
    }
}

// ── Init ─────────────────────────────────────────────────────────────────────
void ui_init() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // ── Left panel (device list) ──────────────────────────────────────────────
    lv_obj_t* left = lv_obj_create(scr);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_size(left, LIST_W, PANEL_H);
    lv_obj_set_style_bg_color(left, lv_color_hex(COL_PANEL_L), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(left, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(left, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(left, 0, LV_PART_MAIN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_obj_create(left);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, LIST_W, 36);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hdr, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr_lbl = lv_label_create(hdr);
    lv_label_set_text(hdr_lbl, "DEVICES");
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(hdr_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    s_list_cont = lv_obj_create(left);
    lv_obj_set_pos(s_list_cont, 0, 36);
    lv_obj_set_size(s_list_cont, LIST_W, PANEL_H - 36);
    lv_obj_set_style_bg_opa(s_list_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_cont, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_cont, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list_cont, LV_SCROLLBAR_MODE_OFF);

    // Vertical divider
    lv_obj_t* div = lv_obj_create(scr);
    lv_obj_set_pos(div, LIST_W, 0);
    lv_obj_set_size(div, 1, PANEL_H);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_DIV), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(div, 0, LV_PART_MAIN);

    // ── Right panel (detail) ──────────────────────────────────────────────────
    lv_obj_t* right = lv_obj_create(scr);
    lv_obj_set_pos(right, DETAIL_X, 0);
    lv_obj_set_size(right, DETAIL_W, PANEL_H);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right, PAD, LV_PART_MAIN);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // ─ Info row (full width) ──────────────────────────────────────────────────
    s_det_title = lv_label_create(right);
    lv_label_set_text(s_det_title, "Tap a device");
    lv_obj_set_style_text_color(s_det_title, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_det_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_det_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_det_mac = lv_label_create(right);
    lv_label_set_text(s_det_mac, "");
    lv_obj_set_style_text_color(s_det_mac, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(s_det_mac, LV_ALIGN_TOP_LEFT, 0, 28);

    s_det_rssi = lv_label_create(right);
    lv_label_set_text(s_det_rssi, "");
    lv_obj_set_style_text_color(s_det_rssi, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_det_rssi, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_det_rssi, LV_ALIGN_TOP_LEFT, 0, 52);

    s_det_seen = lv_label_create(right);
    lv_label_set_text(s_det_seen, "");
    lv_obj_set_style_text_color(s_det_seen, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(s_det_seen, LV_ALIGN_TOP_LEFT, 130, 58);

    // Thin horizontal rule between info and two-column area
    lv_obj_t* rule = lv_obj_create(right);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 80);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_DIV), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rule, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(rule, 0, LV_PART_MAIN);

    // ─ Left column: signal chart ──────────────────────────────────────────────
    lv_obj_t* chart_hdr = lv_label_create(right);
    lv_label_set_text(chart_hdr, "SIGNAL HISTORY");
    lv_obj_set_style_text_color(chart_hdr, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(chart_hdr, LV_ALIGN_TOP_LEFT, 0, 90);

    s_det_chart = lv_chart_create(right);
    lv_obj_set_size(s_det_chart, COL_L_W, 218);
    lv_obj_align(s_det_chart, LV_ALIGN_TOP_LEFT, 0, 110);
    lv_chart_set_type(s_det_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(s_det_chart, LV_CHART_AXIS_PRIMARY_Y, -100, -20);
    lv_chart_set_div_line_count(s_det_chart, 4, 0);
    lv_chart_set_point_count(s_det_chart, 1);
    lv_obj_set_style_bg_color(s_det_chart, lv_color_hex(COL_CHART_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_det_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_det_chart, lv_color_hex(COL_DIV), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_det_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_det_chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_det_chart, lv_color_hex(COL_GRID_LN), LV_PART_MAIN);
    lv_obj_set_style_size(s_det_chart, 0, LV_PART_INDICATOR);  // hide dots

    s_det_series = lv_chart_add_series(s_det_chart,
        lv_color_hex(0x58a6ff), LV_CHART_AXIS_PRIMARY_Y);
    s_chart_pts[0] = LV_CHART_POINT_NONE;
    lv_chart_set_ext_y_array(s_det_chart, s_det_series, s_chart_pts);

    // ─ Right column: 8×8 LED pattern preview ─────────────────────────────────
    lv_obj_t* pat_hdr = lv_label_create(right);
    lv_label_set_text(pat_hdr, "PATTERN");
    lv_obj_set_style_text_color(pat_hdr, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(pat_hdr, LV_ALIGN_TOP_LEFT, COL_R_X, 90);

    // Grid container
    lv_obj_t* grid = lv_obj_create(right);
    lv_obj_set_pos(grid, COL_R_X, 110);
    lv_obj_set_size(grid, 7 * CELL_STP + CELL_SZ, 7 * CELL_STP + CELL_SZ);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            lv_obj_t* cell = lv_obj_create(grid);
            lv_obj_set_pos(cell, c * CELL_STP, r * CELL_STP);
            lv_obj_set_size(cell, CELL_SZ, CELL_SZ);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x111111), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(cell, 2, LV_PART_MAIN);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_grid_cells[r * 8 + c] = cell;
        }
    }

    // Shape name below the grid
    s_det_shape = lv_label_create(right);
    lv_label_set_text(s_det_shape, "");
    lv_obj_set_style_text_color(s_det_shape, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_align(s_det_shape, LV_ALIGN_TOP_LEFT, COL_R_X, 110 + 7 * CELL_STP + CELL_SZ + 8);
}

// ── Public API ────────────────────────────────────────────────────────────────
bool ui_needs_refresh() { return s_needs_refresh; }

void ui_animate() {
    if (!s_anim_active) return;

    uint32_t now = millis();
    float dt = (now - s_anim_last_ms) / 1000.0f;
    if (dt < 0.001f) return;          // called too soon
    if (dt > 0.1f) dt = 0.1f;        // cap after pause or first frame
    s_anim_last_ms = now;

    pulleys::pattern_slot_update(s_anim_slot, dt, now / 1000.0f);

    for (int i = 0; i < 64; i++) {
        lv_obj_set_style_bg_color(s_grid_cells[i],
            lv_color_make(s_led_buf[i].r, s_led_buf[i].g, s_led_buf[i].b),
            LV_PART_MAIN);
    }
}

void ui_refresh() {
    s_needs_refresh = false;
    rebuild_list();

    if (s_selected_idx >= 0) {
        const ArbiterDevice& d = scanner_devices()[s_selected_idx];
        if (d.active) {
            detail_show(&d);
        } else {
            s_selected_idx = -1;
            detail_clear();
        }
    }
}
