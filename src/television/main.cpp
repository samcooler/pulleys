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
#define TV_MARGIN    48      // px kept clear on every side
#define TV_MAX_LINES 12
#define TV_LINE_LEN  128

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

// Largest first; the first that fits the message wins.
static const lgfx::IFont* const FONTS[] = {
    &fonts::DejaVu72, &fonts::DejaVu56, &fonts::DejaVu40,
    &fonts::DejaVu24, &fonts::DejaVu18, &fonts::DejaVu12,
};

// ── Layout ────────────────────────────────────────────────────────────────────
static char  lines[TV_MAX_LINES][TV_LINE_LEN];
static uint8_t lineCount = 0;
static const lgfx::IFont* font = FONTS[0];

// Greedy word wrap of `text` at the current font. False if it does not fit:
// too many lines, too tall, or one word wider than the screen on its own.
static bool wrap(const char* text) {
    const int maxW = LCD_WIDTH - 2 * TV_MARGIN;
    lineCount = 0;
    char cur[TV_LINE_LEN] = "";
    const char* p = text;
    while (true) {
        // Next word, and what ends it.
        while (*p == ' ') p++;
        const char* w = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        size_t wl = p - w;
        if (wl > 0) {
            char trial[TV_LINE_LEN];
            size_t cl = strlen(cur);
            if (cl + 1 + wl >= TV_LINE_LEN) return false;
            snprintf(trial, sizeof(trial), cl ? "%s %.*s" : "%s%.*s", cur, (int)wl, w);
            if (frame.textWidth(trial) <= maxW) {
                strcpy(cur, trial);
            } else {
                if (cl == 0) return false;                    // the word alone is too wide
                if (lineCount >= TV_MAX_LINES) return false;
                strcpy(lines[lineCount++], cur);
                snprintf(cur, sizeof(cur), "%.*s", (int)wl, w);
                if (frame.textWidth(cur) > maxW) return false;
            }
        }
        if (*p == '\n' || *p == 0) {
            if (lineCount >= TV_MAX_LINES) return false;
            strcpy(lines[lineCount++], cur);
            cur[0] = 0;
            if (*p == 0) break;
            p++;
        }
    }
    return lineCount * frame.fontHeight() <= LCD_HEIGHT - 2 * TV_MARGIN;
}

static void layout(const TvMessage& m) {
    for (const lgfx::IFont* f : FONTS) {
        font = f;
        frame.setFont(f);
        if (wrap(m.text)) return;
    }
    Serial.printf("  [TV] \"%s\" does not fit even at the smallest size: clipped\n", m.text);
}

// ── Drawing ───────────────────────────────────────────────────────────────────
static uint16_t dim565(uint32_t rgb, uint16_t level) {   // level 0..256
    uint8_t r = ((rgb >> 16) & 0xFF) * level >> 8;
    uint8_t g = ((rgb >> 8)  & 0xFF) * level >> 8;
    uint8_t b = ( rgb        & 0xFF) * level >> 8;
    return lgfx::color565(r, g, b);
}

static void draw(const TvMessage& m, uint16_t level) {
    uint16_t bg = dim565(m.bg, level);
    frame.fillScreen(bg);
    frame.setFont(font);
    frame.setTextColor(dim565(m.fg, level), bg);
    frame.setTextDatum(lgfx::top_center);
    int lh = frame.fontHeight();
    int y  = (LCD_HEIGHT - lineCount * lh) / 2;
    for (uint8_t i = 0; i < lineCount; i++, y += lh)
        frame.drawString(lines[i], LCD_WIDTH / 2, y);
    frame.pushSprite(0, 0);
}

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

// ── Channels ──────────────────────────────────────────────────────────────────
// A channel is named by its setting value's hash, the way it arrives; 0 is off.
static constexpr uint16_t OFF = 0;

static uint16_t channelOf(uint8_t i) { return pulleys::settings_hash(TV_MESSAGES[i].channel); }

static const char* channelName(uint16_t ch) {
    if (ch == OFF) return TV_OFF;
    for (uint8_t i = 0; i < TV_MESSAGE_COUNT; i++)
        if (channelOf(i) == ch) return TV_MESSAGES[i].channel;
    return "?";
}

static bool channelExists(uint16_t ch) {
    for (uint8_t i = 0; i < TV_MESSAGE_COUNT; i++) if (channelOf(i) == ch) return true;
    return false;
}

// The message after `from` on channel `ch`, wrapping; -1 if the channel has
// none. step = -1 walks backwards. from = -1 starts at the channel's first.
static int8_t stepOn(uint16_t ch, int8_t from, int8_t step = 1) {
    if (ch == OFF) return -1;
    for (uint8_t k = 1; k <= TV_MESSAGE_COUNT; k++) {
        int i = from < 0 ? (step > 0 ? k - 1 : TV_MESSAGE_COUNT - k)
                         : (from + step * (int)k + TV_MESSAGE_COUNT * k) % TV_MESSAGE_COUNT;
        if (channelOf(i) == ch) return (int8_t)i;
    }
    return -1;
}

// What the game wants, read fresh every loop: a change, a reboot and a lapsed
// lease all come down this one path.
static uint16_t wantedChannel() {
    static const uint16_t fallback = pulleys::settings_hash(TV_DEFAULT_CHANNEL);
    static uint16_t warned = 0;
    uint16_t v = pulleys::setting_value(pulleys::settings_hash(TV_SETTING));
    if (v == 0) return fallback;
    if (v == pulleys::settings_hash(TV_OFF)) return OFF;
    if (channelExists(v)) return v;
    if (warned != v) {   // the game names a channel this firmware does not have
        Serial.printf("  [TV] the game asked for a channel (hash %04X) with no messages here: "
                      "playing %s\n", v, TV_DEFAULT_CHANNEL);
        warned = v;
    }
    return fallback;
}

// ── The set ───────────────────────────────────────────────────────────────────
enum TvPhase : uint8_t { TV_SHOW, TV_FADE_OUT, TV_FADE_IN, TV_DARK };

static uint16_t channel   = OFF;      // what is on (or being faded to)
static int8_t   current   = -1;       // message on screen; -1 = dark
static int8_t   nextIdx   = -1;       // where a fade out lands; -1 = dark
static TvPhase  phase     = TV_DARK;
static uint32_t phaseAt   = 0;
static bool     held      = false;
static uint16_t lastLevel = 0xFFFF;

static void goDark(uint32_t now) {
    current = -1;
    phase   = TV_DARK;
    phaseAt = now;
    frame.fillScreen(0);
    frame.pushSprite(0, 0);
    backlight(false);
    Serial.println("  [TV] dark");
}

static void show(int8_t i, uint32_t now) {
    if (i < 0) { goDark(now); return; }
    if (current < 0) backlight(true);
    current = i;
    layout(TV_MESSAGES[i]);
    Serial.printf("  [TV] %s: \"%s\" (%u lines)\n", TV_MESSAGES[i].channel,
                  TV_MESSAGES[i].text, lineCount);
    phase     = TV_FADE_MS ? TV_FADE_IN : TV_SHOW;   // fade the new one up
    phaseAt   = now;
    lastLevel = 0xFFFF;
}

// Fade out of whatever is on, then into message i (or dark). From dark there
// is nothing to fade out of.
static void changeTo(int8_t i, uint32_t now) {
    if (current < 0) { if (i >= 0) show(i, now); return; }
    nextIdx = i;
    if (phase == TV_FADE_OUT) return;            // already on the way down
    // Fading in? Start the fade out from the level it has reached.
    uint32_t t = now - phaseAt;
    phaseAt = (phase == TV_FADE_IN && t < TV_FADE_MS) ? now - (TV_FADE_MS - t) : now;
    phase   = TV_FADE_OUT;
}

static void tune(uint16_t ch, uint32_t now) {
    channel = ch;
    pulleys::device_report(TV_SETTING, channelName(ch));
    Serial.printf("  [TV] channel %s\n", channelName(ch));
    changeTo(stepOn(ch, -1), now);
}

static void tvTick(uint32_t now) {
    static bool tuned = false;   // tune once at boot even to `off`, so it reports
    uint16_t want = wantedChannel();
    if (want != channel || !tuned) { tune(want, now); tuned = true; }
    if (phase == TV_DARK) return;

    const TvMessage& m = TV_MESSAGES[current];
    uint32_t t = now - phaseAt;
    uint16_t level = 256;

    switch (phase) {
        case TV_SHOW: {
            int8_t next = stepOn(channel, current);
            if (!held && next != current && t >= (uint32_t)m.seconds * 1000)
                changeTo(next, now);
            break;
        }
        case TV_FADE_OUT:
            if (t >= TV_FADE_MS) { show(nextIdx, now); return; }
            level = 256 - t * 256 / TV_FADE_MS;
            break;
        case TV_FADE_IN:
            if (t >= TV_FADE_MS) { phase = TV_SHOW; phaseAt = now; break; }
            level = t * 256 / TV_FADE_MS;
            break;
        default:
            break;
    }
    if (level != lastLevel) {
        draw(m, level);
        lastLevel = level;
    }
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
                int8_t i = stepOn(channel, current, buf[0] == 'n' ? 1 : -1);
                if (i < 0) Serial.println("  [TV] the set is off");
                else       changeTo(i, now);
            } else if (buf[0] == 'h') {
                held = !held;
                if (!held && phase == TV_SHOW) phaseAt = now;   // a full hold from here
                Serial.printf("  [TV] %s\n", held ? "holding" : "changing again");
            } else if (buf[0] == 'c') {
                Serial.printf("  [TV] on: %s   default: %s   channels:", channelName(channel),
                              TV_DEFAULT_CHANNEL);
                for (uint8_t i = 0; i < TV_MESSAGE_COUNT; i++) {
                    bool first = true;
                    for (uint8_t j = 0; j < i; j++) if (channelOf(j) == channelOf(i)) first = false;
                    if (first) Serial.printf(" %s", TV_MESSAGES[i].channel);
                }
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
        Serial.printf("── tv channel=%s%s%s  clock=%s\n", channelName(channel),
                      held ? " (held)" : "",
                      pulleys::settings_fresh() ? "" : " (game silent: default)",
                      pulleys::mesh_clock_locked() ? "locked" : "free");
        pulleys::mesh_print_stats();
    }
    delay(TV_FADE_MS ? 5 : 20);
}
