#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_channel.h>
#include <pulleys_mesh.h>
#include "font5x7.h"

// ── Screen — activity display for the sensor mesh ─────────────────────────────
// Listens to deduped sensor EVENTs (and relays them), maintains a per-channel
// activity model, and shows it on the 8×32 matrix in one of two displays:
//
//   COUNTER — per-channel detection counts side by side, each in its channel
//             colour, draining after inactivity
//   RANKING — top-4 channels as 8×8 shape patterns, most active on the left,
//             brightness driven by each channel's activity level
//
// The mode is chosen at boot, not on a timer: the installed matrix has no
// buttons, so each boot uses the mode the previous boot stored and then stores
// the next one. Power-cycling steps COUNTER → RANKING → COUNTER … which is the
// whole user interface.
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

// Counter layout. Digits are 5 wide with a 1px gap, so an n-digit number spans
// 6n-1 of the 32-pixel long axis; channel groups are separated by GROUP_GAP.
// How many channels fit therefore falls out of how big their numbers are —
// four single digits, or two three-digit counters, and so on.
#define DIGIT_W           5
#define DIGIT_GAP         1
#define GROUP_GAP         2
#define MAX_COUNT_CHANS   4
#define COUNT_DISPLAY_MAX 9999   // clamp so one runaway channel cannot crowd the rest

// NVS namespace holding the boot-alternated display mode
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

enum DisplayMode : uint8_t { MODE_COUNTER = 0, MODE_RANKING, MODE_COUNT };
static DisplayMode mode = MODE_COUNTER;   // real value comes from NVS at boot

// Boot-time mode select. Read what the last boot left, then immediately store
// the next mode, so yanking power is what advances the display.
static void loadAndAdvanceMode() {
    Preferences p;
    p.begin(NVS_NS, false);
    uint8_t stored = p.getUChar("mode", MODE_COUNTER);
    if (stored >= MODE_COUNT) stored = MODE_COUNTER;
    mode = (DisplayMode)stored;
    p.putUChar("mode", (uint8_t)((stored + 1) % MODE_COUNT));
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
            if (pulleys::whoami_handle(buf)) continue;   // "?" → PULLEYS-ID line
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
                // Bench override only -- does not touch the stored boot mode.
                mode = (mode == MODE_COUNTER) ? MODE_RANKING : MODE_COUNTER;
                fill_solid(leds, LED_COUNT, CRGB::Black);
                Serial.printf("  mode=%s (this boot only)\n",
                              mode == MODE_COUNTER ? "COUNTER" : "RANKING");
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

    loadAndAdvanceMode();
    Serial.printf("  mode=%s this boot (power-cycle for the other)\n",
                  mode == MODE_COUNTER ? "COUNTER" : "RANKING");
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
    handleSerial();

    // No dwell timer: the mode was chosen at boot and holds until the next
    // power cycle. 'm' on serial still flips it for bench testing.
    updateCounterDrain(now);

    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;

        decayActivity(dt);

        if (mode == MODE_COUNTER) renderCounter(now);
        else                      renderRanking(dt, pulleys::mesh_now_secs());

        FastLED.show();
    }

    if (now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── total=%lu  mode=%s  counts:", totalCount,
                      mode == MODE_COUNTER ? "COUNTER" : "RANKING");
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
