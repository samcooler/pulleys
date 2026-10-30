#include <Arduino.h>
#include <FastLED.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <pulleys_component_display.h>
#include <pulleys_channel.h>

// ── Component state display — charge bars for the game ───────────────────────
// An 8×32 matrix showing one bar per game component: eight vertical bars side
// by side along the long axis, 3 pixels wide with a 1-pixel gap, major
// components 1-5 on the left and final components 1-3 after them. A component
// that is lit shows a full bar, which then drains away until the component
// goes dark.
//
// It draws what the arbiter publishes (MeshComponents) and nothing more: when a
// component counts as lit, and for how long, is the arbiter's rule alone.
// Between packets the bars keep draining by themselves (pulleys_component_display),
// so the animation is smooth and a missed packet costs nothing. If the arbiter
// goes quiet, a pixel at the right-hand end blinks red.
//
// Like every mesh role it relays, beacons the shared clock, answers census and
// supports targeted firmware updates.

#ifndef LED_PIN
  #define LED_PIN   16
#endif
#ifndef LED_COUNT
  #define LED_COUNT 256
#endif
static_assert(LED_COUNT >= pulleys::COMPONENT_DISPLAY_WIDTH * pulleys::COMPONENT_DISPLAY_HEIGHT,
              "the component state display needs the whole 8x32 matrix (256 LEDs)");

#define LED_FPS        60
#define COMPONENT_DISPLAY_BRIGHTNESS_DEFAULT 60   // FastLED global, 0..255
#ifndef COMPONENT_DISPLAY_BRIGHTNESS
  #define COMPONENT_DISPLAY_BRIGHTNESS COMPONENT_DISPLAY_BRIGHTNESS_DEFAULT
#endif
#define TRACK_LEVEL    10             // unlit part of a bar, 0..255 of its colour

// Current cap, as on the screen: 256 pixels flat out would trip a USB port.
#ifndef LED_MAX_MA
  #define LED_MAX_MA 450
#endif

// Which way the panel is mounted. Component 1 is the leftmost bar and bars fill
// upward from the bottom; flip either if they come out the other way round.
#ifndef COMPONENT_DISPLAY_REVERSE_X
  #define COMPONENT_DISPLAY_REVERSE_X 0
#endif
#ifndef COMPONENT_DISPLAY_REVERSE_Y
  #define COMPONENT_DISPLAY_REVERSE_Y 0
#endif

// Each bar wears its component's colour, as the arbiter reports it with every
// charge packet: a major's game1 colour, a final's channel's colour today. So a
// bar matches the ropes that fill it, including after the palette changes.
// These stand in until the arbiter has said (or if it is too old to say): the
// default game's order, finals in game2's purple. A final bar the arbiter sends
// no colour for has no purple channel behind it, and stays dark.
static uint8_t barColor[pulleys::COMPONENT_DISPLAY_BARS] = {
    pulleys::COLOR_RED, pulleys::COLOR_BLUE, pulleys::COLOR_GREEN,
    pulleys::COLOR_YELLOW, pulleys::COLOR_ORANGE,
    pulleys::COLOR_GAME2, pulleys::COLOR_GAME2, pulleys::COLOR_GAME2,
};

static CRGB leds[LED_COUNT];
static pulleys::ComponentDisplayModel model;

static void onComponents(const pulleys::MeshComponents& c, bool relayed) {
    pulleys::component_display_update(model, c.charge, c.decay, millis());
    // An arbiter that predates colours sends none at all, majors included;
    // keep the defaults for it rather than blanking every bar.
    if (c.color[0] != pulleys::MESH_COLOR_UNKNOWN)
        memcpy(barColor, c.color, sizeof(barColor));
    Serial.printf("★ components%s (%.3f%%/s):", relayed ? " (relayed)" : "", c.decay / 1000.0f);
    for (uint8_t i = 0; i < pulleys::COMPONENT_DISPLAY_BARS; i++) Serial.printf(" %3u", c.charge[i]);
    Serial.println();
}

static inline uint16_t ledAt(uint8_t x, uint8_t y) {
    return pulleys::component_display_index(x, y, COMPONENT_DISPLAY_REVERSE_X,
                                            COMPONENT_DISPLAY_REVERSE_Y);
}

static void render() {
    uint32_t now = millis();
    fill_solid(leds, LED_COUNT, CRGB::Black);   // the gaps between bars stay dark
    for (uint8_t bar = 0; bar < pulleys::COMPONENT_DISPLAY_BARS; bar++) {
        if (barColor[bar] >= pulleys::COLOR_COUNT) continue;   // no channel behind it
        uint8_t full, edge;
        pulleys::component_display_pixels(model.shown[bar], full, edge);
        uint8_t x0 = pulleys::component_display_bar_x(bar);
        for (uint8_t h = 0; h < pulleys::COMPONENT_DISPLAY_HEIGHT; h++) {
            CRGB c = pulleys::color_rgb(barColor[bar]);
            if (h < full)       { /* lit */ }
            else if (h == full) c.nscale8(edge > TRACK_LEVEL ? edge : TRACK_LEVEL);
            else                c.nscale8(TRACK_LEVEL);
            for (uint8_t dx = 0; dx < pulleys::COMPONENT_DISPLAY_BAR_W; dx++)
                leds[ledAt(x0 + dx, h)] = c;
        }
    }
    // The arbiter has gone quiet (or never spoke): blink the spare column at the
    // right end, so a dark display can be told from one that is simply waiting
    // for a pull.
    if (pulleys::component_display_stale(model, now) && ((now / 500) & 1))
        leds[ledAt(pulleys::COMPONENT_DISPLAY_WIDTH - 1, 0)] = CRGB(60, 0, 0);
}

// ── Serial console ────────────────────────────────────────────────────────────
//   c<bar> <percent>  show a bar at that level as if the arbiter had sent it, for
//                     the bench: "c0 100" fills component 1, then it drains
static void handleSerial() {
    // Console line input for local display controls.
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;
            if (pulleys::census_handle_serial(buf)) continue;
            if (buf[0] == 'c') {
                int bar = atoi(buf + 1);
                const char* sp = strchr(buf, ' ');
                int pct = sp ? atoi(sp + 1) : 100;
                if (bar >= 0 && bar < pulleys::COMPONENT_DISPLAY_BARS) {
                    uint8_t charge[pulleys::COMPONENT_DISPLAY_BARS];
                    for (uint8_t i = 0; i < pulleys::COMPONENT_DISPLAY_BARS; i++)
                        charge[i] = (uint8_t)(pulleys::component_display_target(model, i, millis()) * 100.0f + 0.5f);
                    charge[bar] = (uint8_t)constrain(pct, 0, 100);
                    pulleys::component_display_update(model, charge, model.decay, millis());
                }
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MA);
    FastLED.setBrightness(COMPONENT_DISPLAY_BRIGHTNESS);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();

    pulleys::identity_init(PULLEYS_TYPE_COMPONENT_STATE_DISPLAY);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Component State Display  %s\n", pulleys::identity_name());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    randomSeed(esp_random());
    pulleys::mesh_init(pulleys::MESH_ORIGIN_COMPONENT_STATE_DISPLAY, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::mesh_on_components(onComponents);
    pulleys::census_set_detail("8x32");
    pulleys::whoami_reply();
    Serial.println("Component state display ready — waiting for the arbiter.\n");
}

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

    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;
        pulleys::component_display_step(model, now, dt);
        render();
        FastLED.show();
    }

    if (now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── display %s\n", pulleys::component_display_stale(model, now) ? "waiting for the arbiter" : "live");
        pulleys::mesh_print_stats();
    }
}
