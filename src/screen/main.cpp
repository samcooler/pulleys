#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_channel.h>
#include <pulleys_panel.h>
#include <pulleys_wave.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <pulleys_component_display.h>
#include "font5x7.h"

// ── Screen — activity display for the sensor mesh ─────────────────────────────
// Listens to deduped sensor EVENTs (and relays them), maintains a per-channel
// activity model, and shows it on the 8×32 matrix in one of two displays:
//
//   COUNTER — per-channel detection counts side by side, each in its channel
//             colour, draining after inactivity
//   RANKING — top-4 channels as 8×8 shape patterns, most active on the left,
//             brightness driven by each channel's activity level
//   ALLON   — four yellow symbols, every pixel lit, ignoring the mesh: the
//             bench display, for looking at the panel itself
//   WAVE    — three wandering sines bubbling in gold on a 14×2 panel (14, skip
//             1, 16) instead of the 8×32 matrix. A different LED surface, so a
//             board is built for it rather than switched into it
//   COMPONENTS — the game's charge bars: one per component, filling when the
//             arbiter says it is lit and draining until it goes dark
//
// Every display runs on the same board and hears the same mesh: every sensor
// event, the arbiter's palette, its component charges. Which one draws is the
// only difference, so any screen can be any of them.
//
// Which one it shows is chosen at boot, not on a timer. A board named in the
// install map (lib/pulleys_install) runs the display assigned to it, so an
// installed screen survives being unplugged without changing what it does.
// A board nobody has decided for falls back to the only interface a matrix with
// no buttons has: each boot uses what the previous boot stored and then stores
// the next, so power-cycling steps COUNTER → RANKING → COUNTER …
//
// This is the whole "game" for v1: ranked activity, and number go up.

#ifndef LED_PIN
  #define LED_PIN   10
#endif
#ifndef LED_COUNT
  #define LED_COUNT 256
#endif

// The display and the LED surface have to agree. COUNTER, RANKING, ALLON and
// COMPONENTS address the whole 8×32 matrix; WAVE addresses a 29-LED 14×2 panel. Mixing
// them would walk off the end of leds[], so an env that pins one display gets
// checked against its own LED_COUNT here rather than at run time.
#ifdef SCREEN_FORCE_DISPLAY
static_assert(SCREEN_FORCE_DISPLAY != pulleys::SCREEN_WAVE ||
              LED_COUNT >= pulleys::PANEL_14X2_LEDS,
              "the wave display needs at least 29 LEDs (14, skip 1, 14)");
static_assert(SCREEN_FORCE_DISPLAY == pulleys::SCREEN_WAVE ||
              LED_COUNT >= 256,
              "the matrix displays need the whole 8x32 (256 LEDs)");
#else
static_assert(LED_COUNT >= 256, "the matrix displays need the whole 8x32 (256 LEDs)");
#endif

#define NUM_CHANNELS   pulleys::CHANNEL_COUNT
#define NUM_SLOTS      4
#define SLOT_ROWS      8      // rows along the long axis, per slot
#define MAT_COLS       8      // short axis
#define MAT_ROWS       32     // long axis
#define LED_FPS        60
#define MAX_BRIGHTNESS 42   // 30% down from 60

// Hard current cap. 256 WS2812Bs can pull ~15 A flat out, which trips a USB
// port and takes the whole bus down with it. FastLED scales global brightness
// to stay under this budget, so a bench run on USB power is safe. Raise it once
// the matrix is on its own 5 V supply.
#ifndef LED_MAX_MA
  #define LED_MAX_MA 450
#endif

// Activity model
#define ACT_PER_EVENT     1.0f    // activity added per detection
#define ACT_HALFLIFE_S    25.0f   // activity decays to half in this long
#define COUNT_IDLE_MS     20000   // no events for this long → counter starts draining
#define COUNT_DRAIN_MS    250     // then drop one per this interval

// Counter layout. Digits are 5 wide with a 1px gap, so an n-digit number spans
// 6n-1 of the 32-pixel long axis; channel groups are separated by GROUP_GAP.
// How many channels fit therefore falls out of how big their numbers are —
// four single digits, or two three-digit counters, and so on.
#define DIGIT_W           5
#define DIGIT_GAP         1
#define GROUP_GAP         2
#define MAX_COUNT_CHANS   4
#define COUNT_DISPLAY_MAX 9999   // clamp so one runaway channel cannot crowd the rest

// All-on display. The four slots show four different symbols in one colour, and
// the shapes' dark gaps are lifted to a dim version of it instead of going out,
// so no pixel on the panel is ever off while this display runs. Brightness is
// the full range rather than MAX_BRIGHTNESS: this is the display to look at a
// panel with, and how bright it actually ends up is LED_MAX_MA's decision.
#define ALLON_COLOR       pulleys::COLOR_YELLOW   // fixed: a hardware check, not a channel
#define ALLON_FLOOR       70     // dim floor, 0..255 of the lit colour
#define ALLON_BRIGHTNESS  255

// NVS namespace holding the boot-alternated display mode (unlisted boards only)
#define NVS_NS            "screen"

// ── Globals ───────────────────────────────────────────────────────────────────
static CRGB leds[LED_COUNT];

struct ChannelState {
    float    activity   = 0.0f;   // decaying EWMA-ish level
    uint32_t count      = 0;      // deduped detections seen on this channel
    uint32_t lastEvent  = 0;
};
static ChannelState chans[NUM_CHANNELS];

static uint32_t totalCount    = 0;   // what the counter displays
static uint32_t lastAnyEvent  = 0;
static uint32_t lastDrainMs   = 0;
static uint8_t  lastActiveCh  = 0;

// Ranking display: 4 PatternSlots reused from the station renderer
static pulleys::PatternSlot patSlots[NUM_SLOTS];
static int8_t  slotChannel[NUM_SLOTS]   = { -1, -1, -1, -1 };
static float   slotBrightness[NUM_SLOTS] = { 0, 0, 0, 0 };  // eased toward target

static uint8_t mode        = pulleys::SCREEN_COUNTER;
static bool    modeIsListed  = false;  // pinned by the install map, not cycled
static bool    forcedByBuild = false;  // pinned by SCREEN_FORCE_DISPLAY instead

// Display select. A board named in DISPLAY_ASSIGNMENT runs what the install map
// says and nothing else — power-cycling it is then just a restart, which is
// what you want of an installed piece someone might unplug by accident.
//
// A board nobody has decided for keeps the old interface: read what the last
// boot left, immediately store the next one, so yanking power steps
// COUNTER → RANKING → COUNTER. On a matrix with no buttons that is still the
// only way to change it in the field.
static void loadDisplayMode() {
#ifdef SCREEN_FORCE_DISPLAY
    // Built to run one display and nothing else ([env:screen_allon]). Beats the
    // install map, because the image is the more deliberate of the two: someone
    // flashed this board on purpose and will flash it back.
    mode         = SCREEN_FORCE_DISPLAY;
    modeIsListed = true;
    forcedByBuild = true;
    return;
#endif
    int8_t listed = pulleys::display_for_device(pulleys::identity_id());
    if (listed >= 0) {
        mode         = (uint8_t)listed;
        modeIsListed = true;
        return;
    }
    Preferences p;
    p.begin(NVS_NS, false);
    uint8_t stored = p.getUChar("mode", pulleys::SCREEN_COUNTER);
    if (stored >= pulleys::SCREEN_CYCLE_COUNT) stored = pulleys::SCREEN_COUNTER;
    mode = stored;
    p.putUChar("mode", (uint8_t)((stored + 1) % pulleys::SCREEN_CYCLE_COUNT));
    p.end();
}

// Events arrive on the WiFi task via mesh_poll() in loop(), so no ISR race here.

// ── Geometry ──────────────────────────────────────────────────────────────────
// Logical (x, y): x = 0..31 along the long axis, y = 0..7 across.
// Physical wiring is serpentine along the long axis, matching the station.
static inline uint16_t xy(uint8_t x, uint8_t y) {
    uint8_t physCol = (x & 1) ? (MAT_COLS - 1 - y) : y;
    return (uint16_t)x * MAT_COLS + physCol;
}

// Channel colors and patterns come from pulleys_channel so a Screen block and
// the Sensor on that channel render the same thing.

// ── Mesh callback ─────────────────────────────────────────────────────────────
static void onMeshEvent(const pulleys::MeshEvent& ev, bool relayed) {
    if (ev.channel >= NUM_CHANNELS) return;
    if (!pulleys::mesh_event_is_pull(ev)) return;   // a sensor confirming a remap
    uint32_t now = millis();

    ChannelState& c = chans[ev.channel];
    c.activity += ACT_PER_EVENT;
    c.count++;
    c.lastEvent = now;

    totalCount++;
    lastAnyEvent = now;
    lastActiveCh = ev.channel;

    Serial.printf("★ ch%-2d from 0x%04X %s mag=%3d ttl=%d  → act=%.2f total=%lu\n",
                  ev.channel, ev.originId, relayed ? "(relayed)" : "(direct) ",
                  ev.magnitude, ev.ttl, c.activity, totalCount);
}

// The arbiter's palette. The counter reads channel colours live every frame;
// ranking caches a pattern per slot, so forget the slots and let the next
// update rebuild them in the new colours.
static void onMeshConfig(const pulleys::MeshConfig& c, bool /*relayed*/) {
    if (!pulleys::palette_set(c.colors)) return;
    pulleys::palette_save();
    if (mode == pulleys::SCREEN_RANKING)
        for (uint8_t s = 0; s < NUM_SLOTS; s++) slotChannel[s] = -1;
    Serial.printf("  [CONFIG] palette from arbiter 0x%04X\n", c.originId);
}

// ── Activity decay ────────────────────────────────────────────────────────────
static void decayActivity(float dt) {
    float k = powf(0.5f, dt / ACT_HALFLIFE_S);
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        chans[i].activity *= k;
        if (chans[i].activity < 0.002f) chans[i].activity = 0.0f;
    }
}

// ── Counter drain: after an idle period, the numbers tick back down ──────────
// Per channel, so a channel that goes quiet empties and drops off the display
// while its busier neighbours keep counting.
static void updateCounterDrain(uint32_t now) {
    if (now - lastDrainMs < COUNT_DRAIN_MS) return;
    lastDrainMs = now;
    for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
        if (chans[c].count == 0) continue;
        if (now - chans[c].lastEvent < COUNT_IDLE_MS) continue;
        chans[c].count--;
    }
    if (totalCount > 0 && now - lastAnyEvent >= COUNT_IDLE_MS) totalCount--;
}

// ── Display: counter ──────────────────────────────────────────────────────────
// One number per observed channel, side by side along the long axis, each in
// its own channel colour. How many fit depends on how wide their numbers are:
// four single digits, two three-digit counters, and so on. Busiest first, so
// the channels that get squeezed off the end are the quiet ones.

// Digits in a value, clamped to what the display is willing to show.
static uint8_t countDigits(uint32_t v) {
    if (v > COUNT_DISPLAY_MAX) v = COUNT_DISPLAY_MAX;
    uint8_t d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

static inline uint8_t numberWidth(uint8_t digits) {
    return digits * (DIGIT_W + DIGIT_GAP) - DIGIT_GAP;
}

// Draw a number with its left edge at x0. Returns the width it occupied.
static uint8_t drawNumber(uint8_t x0, const char* str, const CRGB& col) {
    uint8_t n = strlen(str);
    for (uint8_t d = 0; d < n; d++) {
        const uint8_t* glyph = FONT5X7_DIGITS[str[d] - '0'];
        for (uint8_t gx = 0; gx < DIGIT_W; gx++) {
            uint8_t x = x0 + d * (DIGIT_W + DIGIT_GAP) + gx;
            if (x >= MAT_ROWS) continue;
            uint8_t colBits = glyph[gx];
            for (uint8_t gy = 0; gy < 7; gy++) {
                if (colBits & (1 << gy)) leds[xy(x, gy)] = col;   // rows 0..6, row 7 margin
            }
        }
    }
    return numberWidth(n);
}

// Brightness with a short pulse right after that channel's last event, so a
// tick reads as a change rather than a silent increment.
static uint8_t counterBrightness(uint32_t now, uint32_t lastEvent) {
    float since = (now - lastEvent) / 400.0f;
    if (since >= 1.0f) return MAX_BRIGHTNESS;
    return (uint8_t)(MAX_BRIGHTNESS * (1.0f + 1.6f * (1.0f - since)));
}

static void renderCounter(uint32_t now) {
    fill_solid(leds, LED_COUNT, CRGB::Black);

    // Rank the channels worth showing, most active first. Activity leads;
    // count breaks ties so fully decayed channels still order sensibly.
    uint8_t order[NUM_CHANNELS];
    uint8_t nCand = 0;
    bool    taken[NUM_CHANNELS] = {};
    while (nCand < NUM_CHANNELS) {
        int8_t   best     = -1;
        float    bestAct  = -1.0f;
        uint32_t bestCnt  = 0;
        for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
            if (taken[c] || chans[c].count == 0) continue;
            float act = chans[c].activity;
            if (act > bestAct + 1e-6f ||
                (act > bestAct - 1e-6f && chans[c].count > bestCnt)) {
                bestAct = act; bestCnt = chans[c].count; best = (int8_t)c;
            }
        }
        if (best < 0) break;
        taken[best] = true;
        order[nCand++] = (uint8_t)best;
    }

    // Nothing counted yet — show a single resting zero.
    if (nCand == 0) {
        CRGB col = pulleys::channel_color(lastActiveCh);
        col.nscale8(MAX_BRIGHTNESS);
        drawNumber((MAT_ROWS - numberWidth(1)) / 2, "0", col);
        return;
    }

    // Take as many as the 32-pixel span allows.
    uint8_t nShow = 0, width = 0;
    for (uint8_t i = 0; i < nCand && nShow < MAX_COUNT_CHANS; i++) {
        uint8_t w    = numberWidth(countDigits(chans[order[i]].count));
        uint8_t next = width + (nShow ? GROUP_GAP : 0) + w;
        if (next > MAT_ROWS) break;
        width = next;
        nShow++;
    }
    if (nShow == 0) nShow = 1, width = numberWidth(countDigits(chans[order[0]].count));

    uint8_t x = (MAT_ROWS > width) ? (MAT_ROWS - width) / 2 : 0;
    for (uint8_t i = 0; i < nShow; i++) {
        uint8_t  ch = order[i];
        uint32_t v  = chans[ch].count;
        if (v > COUNT_DISPLAY_MAX) v = COUNT_DISPLAY_MAX;

        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);

        CRGB col = pulleys::channel_color(ch);
        col.nscale8(counterBrightness(now, chans[ch].lastEvent));

        x += drawNumber(x, buf, col) + GROUP_GAP;
    }
}

// ── Slot defaults for the activity displays ──────────────────────────────────
// The resting state of the four pattern slots: shape patterns whose brightness
// the ranking display drives per slot. allOnInit() overwrites this, so it is
// also how the board gets back to ranking after a bench flip out of ALLON.
static void applyActivitySlots() {
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        patSlots[s].buffer     = leds + (uint16_t)s * SLOT_ROWS * MAT_COLS;
        patSlots[s].serpentine = true;
        patSlots[s].maxBri     = 255;   // slot brightness applied separately
        patSlots[s].init(pulleys::PATTERN_SHAPE, SLOT_ROWS, MAT_COLS);
        slotChannel[s]     = -1;
        slotBrightness[s]  = 0.0f;
    }
}

// ── Display: ranking ──────────────────────────────────────────────────────────
// Top-4 channels by activity fill the 4 slots, most active first.
static void updateRanking() {
    // Selection sort of the top NUM_SLOTS channels by activity
    bool taken[NUM_CHANNELS] = {};
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        int8_t best = -1;
        float  bestAct = 0.0f;
        for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
            if (taken[c] || chans[c].activity <= 0.0f) continue;
            if (chans[c].activity > bestAct) { bestAct = chans[c].activity; best = c; }
        }
        if (best >= 0) taken[best] = true;

        if (slotChannel[s] != best) {
            slotChannel[s] = best;
            if (best >= 0) {
                pulleys::channel_slot_init(patSlots[s], best,
                                           leds + (uint16_t)s * SLOT_ROWS * MAT_COLS,
                                           SLOT_ROWS, MAT_COLS, /*serpentine=*/true);
            }
        }
    }
}

static void renderRanking(float dt, float t) {
    updateRanking();

    // Normalize brightness against the busiest channel so the display always
    // uses its full range, with an absolute floor so a quiet field still glows.
    float peak = 0.05f;
    for (uint8_t c = 0; c < NUM_CHANNELS; c++)
        if (chans[c].activity > peak) peak = chans[c].activity;

    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        int8_t ch = slotChannel[s];
        float target = 0.0f;
        if (ch >= 0) {
            float rel = chans[ch].activity / peak;         // 0..1
            target = 0.12f + rel * 0.88f;
        }
        // Ease so rank changes and decay read as motion, not as a hard cut
        slotBrightness[s] += (target - slotBrightness[s]) * (1.0f - expf(-dt * 2.5f));

        CRGB* buf = leds + (uint16_t)s * SLOT_ROWS * MAT_COLS;
        if (ch < 0 && slotBrightness[s] < 0.01f) {
            fill_solid(buf, SLOT_ROWS * MAT_COLS, CRGB::Black);
            continue;
        }

        pulleys::pattern_slot_update(patSlots[s], dt, t);
        uint8_t sc = (uint8_t)(slotBrightness[s] * MAX_BRIGHTNESS);
        for (uint16_t j = 0; j < SLOT_ROWS * MAT_COLS; j++) buf[j].nscale8(sc);
    }
}

// ── Display: all-on ───────────────────────────────────────────────────────────
// No mesh, no activity, no ranking: four symbols in yellow, held forever, with
// every pixel of the panel carrying at least the dim floor. What it answers is
// whether the panel is whole — a dead pixel, a dark block, a colour channel
// that never comes up, a feed that sags under load — none of which a display
// with black in it can show you.
//
// The symbols are four different shapes, so the four 8×8 blocks stay legible as
// blocks rather than merging into one field of yellow.
static const uint8_t ALLON_SHAPE_CH[NUM_SLOTS] = { 1, 2, 3, 4 };

static void allOnInit() {
    // Shape and oscillation come from four different channels; the colour is
    // fixed, so all four read as the same yellow whatever the palette says.
    PulleysCulture yellow =
        pulleys::culture_for(pulleys::COLOR_HUE[ALLON_COLOR], ALLON_SHAPE_CH[0]);
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        pulleys::channel_slot_init(patSlots[s], ALLON_SHAPE_CH[s],
                                   leds + (uint16_t)s * SLOT_ROWS * MAT_COLS,
                                   SLOT_ROWS, MAT_COLS, /*serpentine=*/true);
        patSlots[s].culture.colorA = yellow.colorA;
        patSlots[s].culture.colorB = yellow.colorB;
        patSlots[s].maxBri         = ALLON_BRIGHTNESS;
        slotBrightness[s]          = 1.0f;
    }
    // Ranking re-derives its slots from activity, so forget these channels
    // rather than leave them looking like a ranking it already rendered.
    for (uint8_t s = 0; s < NUM_SLOTS; s++) slotChannel[s] = -1;
}

static void renderAllOn(float dt, float t) {
    CRGB floorCol = pulleys::color_rgb(ALLON_COLOR);
    floorCol.nscale8(ALLON_FLOOR);

    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        pulleys::pattern_slot_update(patSlots[s], dt, t);

        // Lift the shape's dark gaps to the floor instead of letting them go
        // out. Component-wise max, not a blend: the floor is the same hue, so
        // the symbol stays exactly as bright as the pattern drew it.
        CRGB* buf = leds + (uint16_t)s * SLOT_ROWS * MAT_COLS;
        for (uint16_t j = 0; j < SLOT_ROWS * MAT_COLS; j++) {
            if (buf[j].r < floorCol.r) buf[j].r = floorCol.r;
            if (buf[j].g < floorCol.g) buf[j].g = floorCol.g;
            if (buf[j].b < floorCol.b) buf[j].b = floorCol.b;
        }
    }
}

// ── Display: wave ─────────────────────────────────────────────────────────────
// A 14×2 panel — 14 pixels out, one dead pixel at the fold, 14 back — carrying
// three summed sines bubbling around one gold hue. Geometry lives in
// pulleys_panel and the pattern in pulleys_wave, so this is only the wiring
// between them: which surface, at which point on the strip, and what drives it.
//
// The Sensor gets an 8×8 and a 14×2 chained on one output next, which is why
// neither of those two libraries knows anything about a Screen.
static pulleys::Panel      wavePanel;
static pulleys::WaveParams waveCfg;
static pulleys::WaveState  waveState;

// Which way the second run is wired. It is the one thing about a folded strip
// you cannot tell by looking at the code, so it is a build setting here and 'w'
// on serial flips it live — check it once on the bench, then set the default.
#ifndef PANEL_14X2_SERPENTINE
  #define PANEL_14X2_SERPENTINE 1
#endif

static void waveInit() {
    wavePanel = pulleys::panel_14x2(leds, /*origin=*/0,
                                    /*serpentine=*/PANEL_14X2_SERPENTINE);
    waveCfg   = pulleys::WaveParams{};
    waveCfg.maxBri = MAX_BRIGHTNESS;
    pulleys::wave_init(waveState, waveCfg);
    fill_solid(leds, LED_COUNT, CRGB::Black);   // the fold pixel stays dark
}

// dt, not the mesh clock: the pot integrates its own phases, and each board
// bubbles from its own random state. See pulleys_wave on why this one pattern
// deliberately does not match across boards.
static void renderWave(float dt) {
    pulleys::wave_render(wavePanel, waveCfg, waveState, dt);
}

// ── Display: components ───────────────────────────────────────────────────────
// Eight vertical bars along the long axis, 3 pixels wide with a 1-pixel gap:
// the five majors, then the finals. It draws what the arbiter publishes
// (MeshComponents) and nothing more -- when a component counts as lit, and for
// how long, is the arbiter's rule alone. Between packets the bars keep draining
// by themselves (pulleys_component_display), so a missed packet costs nothing.
// If the arbiter goes quiet, a pixel at the right-hand end blinks red.
//
// The model is fed whatever display is up, so switching to this one on the
// bench ('m') shows the game's real state at once rather than empty bars.
//
// Which way the panel is mounted: component 1 is the leftmost bar and bars
// fill upward from the bottom; flip either if they come out the other way.
#ifndef COMPONENT_DISPLAY_REVERSE_X
  #define COMPONENT_DISPLAY_REVERSE_X 0
#endif
#ifndef COMPONENT_DISPLAY_REVERSE_Y
  #define COMPONENT_DISPLAY_REVERSE_Y 0
#endif
#define COMPONENT_BRIGHTNESS 60   // of 255; what the bars' own board used to run at
#define TRACK_LEVEL          10   // unlit part of a bar, 0..255 of its colour

static pulleys::ComponentDisplayModel compModel;

// Each bar wears its component's colour, as the arbiter reports it with every
// charge packet: a major's game1 colour, a final's purple. These stand in until
// the arbiter has said (or if it is too old to say). A final bar the arbiter
// sends no colour for has no purple channel behind it, and stays dark.
static uint8_t barColor[pulleys::COMPONENT_DISPLAY_BARS] = {
    pulleys::COLOR_RED, pulleys::COLOR_BLUE, pulleys::COLOR_GREEN,
    pulleys::COLOR_YELLOW, pulleys::COLOR_ORANGE,
    pulleys::COLOR_GAME2, pulleys::COLOR_GAME2, pulleys::COLOR_GAME2,
};
static_assert(pulleys::COMPONENT_DISPLAY_BARS == pulleys::MESH_COMPONENT_COUNT,
              "one bar per component in the arbiter's packet");

static void onComponents(const pulleys::MeshComponents& c, bool relayed) {
    pulleys::component_display_update(compModel, c.charge, c.decay, millis());
    // An arbiter that predates colours sends none at all, majors included;
    // keep the defaults for it rather than blanking every bar.
    if (c.color[0] != pulleys::MESH_COLOR_UNKNOWN)
        memcpy(barColor, c.color, sizeof(barColor));
    if (mode != pulleys::SCREEN_COMPONENTS) return;
    Serial.printf("★ components%s (%.3f%%/s):", relayed ? " (relayed)" : "", c.decay / 1000.0f);
    for (uint8_t i = 0; i < pulleys::COMPONENT_DISPLAY_BARS; i++) Serial.printf(" %3u", c.charge[i]);
    Serial.println();
}

static inline uint16_t barLed(uint8_t x, uint8_t y) {
    return pulleys::component_display_index(x, y, COMPONENT_DISPLAY_REVERSE_X,
                                            COMPONENT_DISPLAY_REVERSE_Y);
}

static void renderComponents(uint32_t now, float dt) {
    pulleys::component_display_step(compModel, now, dt);
    fill_solid(leds, LED_COUNT, CRGB::Black);   // the gaps between bars stay dark
    for (uint8_t bar = 0; bar < pulleys::COMPONENT_DISPLAY_BARS; bar++) {
        if (barColor[bar] >= pulleys::COLOR_COUNT) continue;   // no channel behind it
        uint8_t full, edge;
        pulleys::component_display_pixels(compModel.shown[bar], full, edge);
        uint8_t x0 = pulleys::component_display_bar_x(bar);
        for (uint8_t h = 0; h < pulleys::COMPONENT_DISPLAY_HEIGHT; h++) {
            CRGB c = pulleys::color_rgb(barColor[bar]);
            c.nscale8(COMPONENT_BRIGHTNESS);
            if (h < full)       { /* lit */ }
            else if (h == full) c.nscale8(edge > TRACK_LEVEL ? edge : TRACK_LEVEL);
            else                c.nscale8(TRACK_LEVEL);
            for (uint8_t dx = 0; dx < pulleys::COMPONENT_DISPLAY_BAR_W; dx++)
                leds[barLed(x0 + dx, h)] = c;
        }
    }
    if (pulleys::component_display_stale(compModel, now) && ((now / 500) & 1))
        leds[barLed(pulleys::COMPONENT_DISPLAY_WIDTH - 1, 0)] = CRGB(60, 0, 0);
}

// ── Serial console — inject fake events to test the display standalone ────────
//   e<ch>  simulate one detection on that channel
//   c<bar> <percent>  set a component bar as if the arbiter had sent it
//             ("c0 100" fills component 1, then it drains)
//   r      reset all activity and the counter
//   m      force the other display mode now (refused on a board built for one)
//   w      flip the 14×2 panel's row direction, for matching the wiring
//   h<c>,<s>  set the wave's centre hue and its swing (0-255), for finding the
//             arc on the bench before writing it into WaveParams
static void handleSerial() {
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;   // "?" → PULLEYS-ID line
            if (pulleys::census_handle_serial(buf)) continue; // "CENSUS" → who is out there
            if (buf[0] == 'e') {
                int ch = atoi(buf + 1);
                if (ch >= 0 && ch < NUM_CHANNELS) {
                    pulleys::MeshEvent ev = {};
                    ev.channel   = ch;
                    ev.originId  = 0xFACE;   // marker for injected test events
                    ev.magnitude = 90;
                    ev.ttl       = pulleys::MESH_TTL_START;
                    onMeshEvent(ev, false);
                }
            } else if (buf[0] == 'c') {
                int bar = atoi(buf + 1);
                const char* sp = strchr(buf, ' ');
                int pct = sp ? atoi(sp + 1) : 100;
                if (bar >= 0 && bar < pulleys::COMPONENT_DISPLAY_BARS) {
                    uint8_t charge[pulleys::COMPONENT_DISPLAY_BARS];
                    for (uint8_t i = 0; i < pulleys::COMPONENT_DISPLAY_BARS; i++)
                        charge[i] = (uint8_t)(pulleys::component_display_target(compModel, i, millis()) * 100.0f + 0.5f);
                    charge[bar] = (uint8_t)constrain(pct, 0, 100);
                    pulleys::component_display_update(compModel, charge, compModel.decay, millis());
                }
            } else if (buf[0] == 'r') {
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) chans[i] = ChannelState{};
                totalCount = 0;
                Serial.println("  reset");
            } else if (buf[0] == 'x') {          // broadcast a test event onto the mesh
                pulleys::mesh_send_event(7, 0, 90, 0);
                Serial.println("  [TX] test broadcast");
            } else if (buf[0] == 'h') {
                int c = -1, sw = -1;
                if (sscanf(buf + 1, "%d,%d", &c, &sw) == 2 &&
                    c >= 0 && c < 256 && sw >= 0 && sw < 128) {
                    waveCfg.centerHue = (uint8_t)c;
                    waveCfg.hueSwing  = (uint8_t)sw;
                }
                Serial.printf("  wave hue: centre=%u swing=±%u "
                              "(put an arc you like in WaveParams)\n",
                              waveCfg.centerHue, waveCfg.hueSwing);
            } else if (buf[0] == 'w') {
                wavePanel.serpentine = !wavePanel.serpentine;
                Serial.printf("  14x2 rows: %s (set PANEL_14X2_SERPENTINE to keep it)\n",
                              wavePanel.serpentine ? "serpentine" : "straight");
            } else if (buf[0] == 'm' && forcedByBuild) {
                // A board built for one display has an LED surface to match it;
                // cycling to a matrix display on a 29-LED strip would render
                // off the end of the array. Reflash to change it.
                Serial.printf("  display=%s is built in — reflash to change it\n",
                              pulleys::screen_display_name(mode));
            } else if (buf[0] == 'm') {
                // Bench override only -- does not touch the stored boot mode.
                mode = (mode + 1) % pulleys::SCREEN_DISPLAY_COUNT;
                pulleys::census_set_detail(pulleys::screen_display_name(mode));
                // 'm' reaches every display, ALLON included -- unlike the boot
                // cycle, which stops short of it.
                if (mode == pulleys::SCREEN_ALLON) allOnInit();
                else                               applyActivitySlots();
                fill_solid(leds, LED_COUNT, CRGB::Black);
                Serial.printf("  display=%s (this boot only)\n",
                              pulleys::screen_display_name(mode));
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MA);
    FastLED.setBrightness(255);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();

    pulleys::identity_init(PULLEYS_TYPE_SCREEN);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Screen  %s\n", pulleys::identity_name());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    randomSeed(esp_random());
    pulleys::palette_load();
    applyActivitySlots();

    pulleys::mesh_init(pulleys::MESH_ORIGIN_SCREEN, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::mesh_on_event(onMeshEvent);
    pulleys::mesh_on_config(onMeshConfig);
    pulleys::mesh_on_components(onComponents);

    loadDisplayMode();
    if      (mode == pulleys::SCREEN_ALLON) allOnInit();
    else if (mode == pulleys::SCREEN_WAVE)  waveInit();
    pulleys::census_set_detail(pulleys::screen_display_name(mode));
    Serial.printf("  display=%s (%s)\n", pulleys::screen_display_name(mode),
                  forcedByBuild ? "built in — reflash to change it"
                  : modeIsListed ? "from the install map"
                                 : "unlisted — power-cycle for the next one");
    lastAnyEvent = millis();
    pulleys::whoami_reply();
    Serial.println("Screen ready — listening for sensor events.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastLed = 0;
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    pulleys::census_poll();

    // A targeted update owns the board while it runs, and
    // FastLED.show() turns interrupts off for long enough to cost packets.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

    handleSerial();

    // No dwell timer: the mode was chosen at boot and holds until the next
    // power cycle. 'm' on serial still flips it for bench testing.
    updateCounterDrain(now);

    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;

        decayActivity(dt);

        if      (mode == pulleys::SCREEN_COUNTER) renderCounter(now);
        else if (mode == pulleys::SCREEN_ALLON)   renderAllOn(dt, pulleys::mesh_now_secs());
        else if (mode == pulleys::SCREEN_WAVE)    renderWave(dt);
        else if (mode == pulleys::SCREEN_COMPONENTS) renderComponents(now, dt);
        else                                      renderRanking(dt, pulleys::mesh_now_secs());

        FastLED.show();
    }

    if (now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── total=%lu  display=%s  counts:", totalCount,
                      pulleys::screen_display_name(mode));
        bool any = false;
        for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
            if (chans[c].count == 0) continue;
            Serial.printf("  ch%d=%lu(%.2f)", c, (unsigned long)chans[c].count,
                          chans[c].activity);
            any = true;
        }
        if (!any) Serial.print("  --");
        Serial.println();
        Serial.printf("  [SYNC] clock=%s meshNow=%lums\n",
                      pulleys::mesh_clock_locked() ? "locked" : "free",
                      (unsigned long)pulleys::mesh_now());
        pulleys::mesh_print_stats();
    }
}
