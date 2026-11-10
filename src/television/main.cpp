#include <Arduino.h>
#include <Wire.h>
#include <pulleys_lcd.h>
#include <pulleys_ch422g.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <pulleys_install.h>
#include <pulleys_settings.h>
#include <pulleys_textcard.h>
#include "messages.h"

// ── Television — the TV set inside the diorama ────────────────────────────────
// A Screen on the mesh, but not the screen image: it runs on an arbiter's
// 800×480 LCD board (lib/pulleys_lcd) instead of an LED surface, so it is its
// own firmware. It shows text on a solid colour, one message after another,
// fading through black between them.
//
// The game drives it with one device setting (pulleys_settings),
// television.channel: which channel of messages.h to play, or `dark` for a
// dark screen with the backlight out. A change fades straight to the new
// channel's first message. Unset -- no game, or the arbiter silent past the
// lease -- plays TV_DEFAULT_CHANNEL. It reports back the channel it is
// actually showing, so the arbiter can show it as fact.
//
// On the mesh it is an ordinary Screen: it relays, answers census (detail
// "television"), and takes firmware updates.
//
// Console:
//   n      change to the next message on this channel now
//   b      back to the previous one
//   h      hold on this message / resume changing
//   c      list the channels
//   S television.channel=<name>  as if the arbiter had set it (lapses with the lease)
//   ?      PULLEYS-ID line;  CENSUS  who is out there

#define TV_SETTING   "television.channel"
#define TV_OFF       "dark"

#ifndef TV_FADE_MS
#define TV_FADE_MS   350     // each way, through black; 0 = a hard cut
#endif

#if defined(ARBITER_BOARD_CROWPANEL7)
#define PANEL_MCU_ADDR 0x30  // STC8: 0 = full backlight ... 245 = off
#define I2C_SDA 15
#define I2C_SCL 16
#else
#define I2C_SDA 8            // the bus the CH422G expander sits on
#define I2C_SCL 9
#endif

static pulleys::LcdPanel display;
static LGFX_Sprite       frame(&display);   // drawn whole, then pushed: no half-drawn frames
static pulleys::CardPlayer player(frame, TV_MESSAGES, TV_MESSAGE_COUNT, TV_FADE_MS);

// ── Backlight ─────────────────────────────────────────────────────────────────
// `dark` should look off: a black frame on a lit LCD still glows in a dark box.
#if !defined(ARBITER_BOARD_CROWPANEL7)
static pulleys::CH422G expander;
#endif

static bool backlight(bool on) {
#if defined(ARBITER_BOARD_CROWPANEL7)
    Wire.beginTransmission(PANEL_MCU_ADDR);
    Wire.write(on ? 0 : 245);
    return Wire.endTransmission() == 0;
#else
    return expander.set(pulleys::CH422G_EXIO_DISP, on);
#endif
}

// ── Channel ───────────────────────────────────────────────────────────────────
// What the game wants, read fresh every loop: a change, a reboot and a lapsed
// lease all come down this one path.
static uint16_t wantedChannel() {
    static const uint16_t fallback = pulleys::settings_hash(TV_DEFAULT_CHANNEL);
    static uint16_t warned = 0;
    uint16_t v = pulleys::setting_value(pulleys::settings_hash(TV_SETTING));
    if (v == 0) return fallback;
    if (v == pulleys::settings_hash(TV_OFF)) return pulleys::CARD_DARK;
    if (player.hasChannel(v)) return v;
    if (warned != v) {   // the game names a channel this firmware does not have
        Serial.printf("  [TV] the game asked for a channel (hash %04X) with no messages here: "
                      "playing %s\n", v, TV_DEFAULT_CHANNEL);
        warned = v;
    }
    return fallback;
}

static void tvTick(uint32_t now) {
    static bool tuned = false;   // tune once at boot even to dark, so it reports
    uint16_t want = wantedChannel();
    if (want != player.channel() || !tuned) {
        tuned = true;
        player.tune(want, now);
        const char* name = player.channelName(want, TV_OFF);
        pulleys::device_report(TV_SETTING, name);
        Serial.printf("  [TV] channel %s\n", name);
    }
    player.tick(now);
}

// ── Console ───────────────────────────────────────────────────────────────────
static void handleSerial() {
    static char buf[64];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;        // "?" → PULLEYS-ID line
            if (pulleys::census_handle_serial(buf)) continue; // "CENSUS"
            uint32_t now = millis();
            if (buf[0] == 'n' || buf[0] == 'b') {
                if (!player.step(buf[0] == 'n' ? 1 : -1, now)) Serial.println("  [TV] the set is dark");
            } else if (buf[0] == 'h') {
                player.hold(!player.held(), now);
                Serial.printf("  [TV] %s\n", player.held() ? "holding" : "changing again");
            } else if (buf[0] == 'c') {
                Serial.printf("  [TV] on: %s   default: %s   channels:",
                              player.channelName(player.channel(), TV_OFF), TV_DEFAULT_CHANNEL);
                player.printChannels(Serial);
                Serial.println(" " TV_OFF);
            } else if (buf[0] == 'S' && strchr(buf, '=')) {
                char* eq = strchr(buf, '=');
                *eq = 0;
                const char* key = buf + 1;
                while (*key == ' ') key++;
                pulleys::settings_pretend(key, eq + 1);
                Serial.printf("  [SETTINGS] %s=%s (for %lus, unless the arbiter says otherwise)\n",
                              key, eq + 1, (unsigned long)(pulleys::SETTINGS_LEASE_MS / 1000));
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

    // Backlight and panel reset are on the expander (or the STC8 on the
    // CrowPanel). The set starts dark; tuning to the first channel lights it.
    Wire.begin(I2C_SDA, I2C_SCL, 400000);
#if defined(ARBITER_BOARD_CROWPANEL7)
    bool bl = backlight(false);
#else
    bool bl = expander.init() && backlight(false);
#endif

    bool ok = display.init();
    display.setRotation(0);
    display.fillScreen(0);
    frame.setColorDepth(16);
    frame.setPsram(true);
    bool fb = frame.createSprite(LCD_WIDTH, LCD_HEIGHT) != nullptr;
    player.onDark([](bool dark) {
        backlight(!dark);
        Serial.printf("  [TV] %s\n", dark ? "dark" : "lit");
    });

    pulleys::identity_init(PULLEYS_TYPE_SCREEN);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Television  %s\n", pulleys::identity_name());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  display %s, frame buffer %s, backlight %s\n",
                  ok ? "OK" : "FAILED", fb ? "OK" : "FAILED (no PSRAM?)",
                  bl ? "OK" : "NOT ACKED");

    pulleys::mesh_init(pulleys::MESH_ORIGIN_SCREEN, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::settings_init();
    pulleys::census_set_detail(pulleys::screen_display_name(pulleys::SCREEN_TELEVISION));

    pulleys::whoami_reply();
    Serial.printf("Television ready — %u messages, default channel %s.\n\n",
                  TV_MESSAGE_COUNT, TV_DEFAULT_CHANNEL);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    pulleys::mesh_poll();
    pulleys::census_poll();

    // A targeted update owns the board while it runs; the set holds its frame.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

    handleSerial();
    tvTick(now);
    pulleys::settings_poll();

    static uint32_t lastLog = 0;
    if (now - lastLog >= 10000) {
        lastLog = now;
        Serial.printf("── tv channel=%s%s%s  clock=%s\n",
                      player.channelName(player.channel(), TV_OFF),
                      player.held() ? " (held)" : "",
                      pulleys::settings_fresh() ? "" : " (game silent: default)",
                      pulleys::mesh_clock_locked() ? "locked" : "free");
        pulleys::mesh_print_stats();
    }
    delay(TV_FADE_MS ? 5 : 20);
}
