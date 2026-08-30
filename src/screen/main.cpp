#include <Arduino.h>
#include <FastLED.h>
#include <pulleys_identity.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_channel.h>
#include <pulleys_mesh.h>
#include "font5x7.h"

// ── Screen — activity display for the sensor mesh ─────────────────────────────
// Listens to deduped sensor EVENTs (and relays them), maintains a per-channel
// activity model, and shows it on the 8×32 matrix in two alternating displays:
//
//   COUNTER — arabic numerals counting detections, draining after inactivity
//   RANKING — top-4 channels as 8×8 shape patterns, most active on the left,
//             brightness driven by each channel's activity level
//
// This is the whole "game" for v1: ranked activity, and number go up.

#ifndef LED_PIN
  #define LED_PIN   10
#endif
#ifndef LED_COUNT
  #define LED_COUNT 256
#endif

#define NUM_CHANNELS   16
#define NUM_SLOTS      4
#define SLOT_ROWS      8      // rows along the long axis, per slot
#define MAT_COLS       8      // short axis
#define MAT_ROWS       32     // long axis
#define LED_FPS        60
#define MAX_BRIGHTNESS 60

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

// Display cycling
#define MODE_COUNTER_MS   18000
#define MODE_RANKING_MS   22000

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

enum DisplayMode : uint8_t { MODE_COUNTER = 0, MODE_RANKING };
static DisplayMode mode        = MODE_RANKING;
static uint32_t    modeStartMs = 0;

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

// ── Activity decay ────────────────────────────────────────────────────────────
static void decayActivity(float dt) {
    float k = powf(0.5f, dt / ACT_HALFLIFE_S);
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        chans[i].activity *= k;
        if (chans[i].activity < 0.002f) chans[i].activity = 0.0f;
    }
}

// ── Counter drain: after an idle period, the number ticks back down ───────────
static void updateCounterDrain(uint32_t now) {
    if (totalCount == 0) return;
    if (now - lastAnyEvent < COUNT_IDLE_MS) return;
    if (now - lastDrainMs < COUNT_DRAIN_MS) return;
    lastDrainMs = now;
    totalCount--;
}

// ── Display: counter ──────────────────────────────────────────────────────────
// Digits are drawn along the long axis, 5 columns wide + 1 space, so up to 5
// digits fit across the 32-pixel span. The number is right-aligned.
static void renderCounter(uint32_t now) {
    fill_solid(leds, LED_COUNT, CRGB::Black);

    char buf[8];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)totalCount);
    uint8_t n = strlen(buf);
    if (n > 5) { // clamp — show the low 5 digits
        memmove(buf, buf + (n - 5), 6);
        n = 5;
    }

    uint8_t width = n * 5 + (n - 1);           // digits + 1px gaps
    uint8_t x0    = (MAT_ROWS - width) / 2;    // centered along the long axis

    // Gentle pulse right after an event so the tick reads as a change
    float since = (now - lastAnyEvent) / 400.0f;
    uint8_t bri = MAX_BRIGHTNESS;
    if (since < 1.0f) bri = (uint8_t)(MAX_BRIGHTNESS * (1.0f + 1.6f * (1.0f - since)));

    CRGB col = pulleys::channel_color(lastActiveCh);
    col.nscale8(bri);

    for (uint8_t d = 0; d < n; d++) {
        const uint8_t* glyph = FONT5X7_DIGITS[buf[d] - '0'];
        for (uint8_t gx = 0; gx < 5; gx++) {
            uint8_t x = x0 + d * 6 + gx;
            if (x >= MAT_ROWS) continue;
            uint8_t colBits = glyph[gx];
            for (uint8_t gy = 0; gy < 7; gy++) {
                if (colBits & (1 << gy)) leds[xy(x, gy)] = col;   // rows 0..6, row 7 margin
            }
        }
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

// ── Serial console — inject fake events to test the display standalone ────────
//   e<ch>  simulate one detection on that channel
//   r      reset all activity and the counter
//   m      force the other display mode now
static void handleSerial() {
    static char buf[16];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
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
            } else if (buf[0] == 'r') {
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) chans[i] = ChannelState{};
                totalCount = 0;
                Serial.println("  reset");
            } else if (buf[0] == 'x') {          // broadcast a test event onto the mesh
                pulleys::mesh_send_event(7, 0, 90, 0);
                Serial.println("  [TX] test broadcast");
            } else if (buf[0] == 'm') {
                mode = (mode == MODE_COUNTER) ? MODE_RANKING : MODE_COUNTER;
                modeStartMs = millis();
                fill_solid(leds, LED_COUNT, CRGB::Black);
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
    for (uint8_t s = 0; s < NUM_SLOTS; s++) {
        patSlots[s].buffer     = leds + (uint16_t)s * SLOT_ROWS * MAT_COLS;
        patSlots[s].serpentine = true;
        patSlots[s].maxBri     = 255;   // slot brightness applied separately
        patSlots[s].init(pulleys::PATTERN_SHAPE, SLOT_ROWS, MAT_COLS);
    }

    pulleys::mesh_init(pulleys::MESH_ORIGIN_SCREEN, pulleys::identity_id());
    pulleys::mesh_on_event(onMeshEvent);

    modeStartMs  = millis();
    lastAnyEvent = millis();
    Serial.println("Screen ready — listening for sensor events.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastLed = 0;
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    handleSerial();

    // Alternate displays
    uint32_t dwell = (mode == MODE_COUNTER) ? MODE_COUNTER_MS : MODE_RANKING_MS;
    if (now - modeStartMs >= dwell) {
        mode = (mode == MODE_COUNTER) ? MODE_RANKING : MODE_COUNTER;
        modeStartMs = now;
        fill_solid(leds, LED_COUNT, CRGB::Black);
    }

    updateCounterDrain(now);

    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;

        decayActivity(dt);

        if (mode == MODE_COUNTER) renderCounter(now);
        else                      renderRanking(dt, now / 1000.0f);

        FastLED.show();
    }

    if (now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── total=%lu  mode=%s  ranking:", totalCount,
                      mode == MODE_COUNTER ? "COUNTER" : "RANKING");
        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            if (slotChannel[s] < 0) Serial.print("  --");
            else Serial.printf("  ch%d(%.2f)", slotChannel[s], chans[slotChannel[s]].activity);
        }
        Serial.println();
        pulleys::mesh_print_stats();
    }
}
