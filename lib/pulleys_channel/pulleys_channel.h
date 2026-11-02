#pragma once

#include <stdint.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_protocol.h>
#include <pulleys_patterns.h>
#include <pulleys_install.h>   // channel_for_device / channel_from_id live here now

// ── pulleys_channel — the visual identity of a channel ────────────────────────
//
// A channel's shape is derived from its number and its colour from the palette
// below, which every node holds the same copy of, so a Sensor on the rope and
// that channel's block on a Screen render the identical pattern. That match is
// the point: it's how someone sees which block on the array belongs to the
// thing they just pulled.
//
// Both roles MUST get their colors and shape from here — duplicating the
// derivation is how the two ends silently drift apart.

namespace pulleys {

// ── Colours ───────────────────────────────────────────────────────────────────
// A channel's colour is not derived from its number any more: it is one of six
// named colours, and which channel wears which is set on the arbiter (possibly
// daily) and broadcast to every node as MESH_MSG_CONFIG. Six rather than one
// per channel because the game asks for colours, not ropes — several ropes
// sharing a colour is the normal case, and the colours must be few enough to
// call out loud.
//
// The game splits them in two: the first five are game1's (Simon picks one to
// five of them to ask for), and purple is game2's. Keep that order — the game
// code relies on game1 being indices 0..COLOR_GAME1_COUNT-1.
//
// What each colour actually is, as RGB, is the arbiter's too: it can be tuned
// on site (TUNE RGB) and travels with the palette. COLOR_DEFAULT_HUE is only
// where a board starts before it has heard one.
//
// Blue and purple sit in the deep-blue band (hue ~150–200) the old palette left
// out because eyes focus it worst at night. They are here because the piece
// asks for them; if they smear on the rope, tune them, not the list.
enum ChannelColor : uint8_t {
    COLOR_RED = 0, COLOR_BLUE, COLOR_GREEN, COLOR_YELLOW, COLOR_ORANGE,
    COLOR_PURPLE,
    COLOR_COUNT
};
static constexpr uint8_t COLOR_GAME1_COUNT = 5;            // red .. orange
static constexpr uint8_t COLOR_GAME2       = COLOR_PURPLE;

static constexpr uint8_t COLOR_DEFAULT_HUE[COLOR_COUNT] = {
    0, 160, 96, 60, 28, 192          // red, blue, green, yellow, orange, purple
};

// A colour's factory RGB: its default hue, slightly desaturated, full value.
inline CRGB color_default_rgb(uint8_t color) {
    return CHSV(COLOR_DEFAULT_HUE[color % COLOR_COUNT], 235, 255);
}

inline const char* color_name(uint8_t c) {
    static const char* const N[COLOR_COUNT] = {
        "red", "blue", "green", "yellow", "orange", "purple" };
    return c < COLOR_COUNT ? N[c] : "?";
}

// Colour for a channel nobody has set: walk the list from channel 1, so a fresh
// install gets every colour before any repeats. Channel 0 is the fault channel
// (see pulleys_install.h) and lands on purple only by that arithmetic.
inline uint8_t default_channel_color(uint8_t ch) {
    return (uint8_t)((ch + COLOR_COUNT - 1) % COLOR_COUNT);
}

// ── The palette: channel -> colour, colour -> RGB ─────────────────────────────
// One shared pair of tables per program, behind a function-local static for the
// reason pulleys_mesh gives: several translation units include this header, and
// a namespace-scope static would give each its own copy.
namespace _palette {
struct Tables {
    uint8_t chan[CHANNEL_COUNT];
    CRGB    rgb[COLOR_COUNT];
    Tables() {
        for (uint8_t i = 0; i < CHANNEL_COUNT; i++) chan[i] = default_channel_color(i);
        for (uint8_t c = 0; c < COLOR_COUNT; c++)   rgb[c]  = color_default_rgb(c);
    }
};
inline Tables& T() { static Tables t; return t; }
inline uint8_t* table() { return T().chan; }
}  // namespace _palette

inline uint8_t channel_color_index(uint8_t ch) {
    return _palette::table()[ch % CHANNEL_COUNT];
}
inline const uint8_t* palette_get() { return _palette::table(); }

// Adopt a whole palette. Out-of-range entries keep their current colour rather
// than wrapping onto some other one. True if anything changed, which is the
// caller's cue to save and re-point whatever it is drawing.
inline bool palette_set(const uint8_t colors[CHANNEL_COUNT]) {
    uint8_t* t = _palette::table();
    bool changed = false;
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
        if (colors[i] >= COLOR_COUNT || colors[i] == t[i]) continue;
        t[i] = colors[i];
        changed = true;
    }
    return changed;
}

inline bool channel_set_color(uint8_t ch, uint8_t color) {
    if (ch >= CHANNEL_COUNT || color >= COLOR_COUNT) return false;
    uint8_t* t = _palette::table();
    if (t[ch] == color) return false;
    t[ch] = color;
    return true;
}

inline CRGB color_rgb(uint8_t color) { return _palette::T().rgb[color % COLOR_COUNT]; }

inline bool color_set_rgb(uint8_t color, CRGB rgb) {
    if (color >= COLOR_COUNT) return false;
    CRGB& cur = _palette::T().rgb[color];
    if (cur == rgb) return false;
    cur = rgb;
    return true;
}

// Every colour's RGB as COLOR_COUNT r,g,b triples: the wire and NVS form.
inline void colors_get_rgb(uint8_t out[COLOR_COUNT * 3]) {
    for (uint8_t c = 0; c < COLOR_COUNT; c++) {
        const CRGB& v = _palette::T().rgb[c];
        out[c * 3] = v.r; out[c * 3 + 1] = v.g; out[c * 3 + 2] = v.b;
    }
}

// Adopt every colour's RGB at once. True if anything changed.
inline bool colors_set_rgb(const uint8_t in[COLOR_COUNT * 3]) {
    bool changed = false;
    for (uint8_t c = 0; c < COLOR_COUNT; c++)
        changed |= color_set_rgb(c, CRGB(in[c * 3], in[c * 3 + 1], in[c * 3 + 2]));
    return changed;
}

// NVS, namespace "palette". A board that has never heard an arbiter keeps the
// defaults; one that has keeps the last palette it heard across reboots, so a
// rope comes up in today's colour even before the arbiter's next repeat.
inline void palette_load() {
    Preferences p;
    p.begin("palette", true);
    uint8_t buf[CHANNEL_COUNT];
    size_t n = p.getBytes("col", buf, sizeof(buf));
    uint8_t rgb[COLOR_COUNT * 3];
    size_t m = p.getBytes("rgb", rgb, sizeof(rgb));
    p.end();
    if (n == sizeof(buf)) palette_set(buf);
    if (m == sizeof(rgb)) colors_set_rgb(rgb);
}

inline void palette_save() {
    uint8_t rgb[COLOR_COUNT * 3];
    colors_get_rgb(rgb);
    Preferences p;
    p.begin("palette", false);
    p.putBytes("col", _palette::table(), CHANNEL_COUNT);
    p.putBytes("rgb", rgb, sizeof(rgb));
    p.end();
}

inline CRGB channel_color(uint8_t ch) { return color_rgb(channel_color_index(ch)); }

// The hue of a channel's colour, for whatever is built around a hue (the
// sensor's bar bubbles in an arc centred on it).
inline uint8_t channel_hue(uint8_t ch) { return rgb2hsv_approximate(channel_color(ch)).hue; }

// Shapes that read as one colour: a lit body with a moving edge, legible even
// if the accent colour never appeared. CHECKER, POLKA and QUADRANTS are left
// out because they only work when two colours alternate across the form —
// which is exactly the complexity a rope block at a distance cannot carry.
static constexpr uint8_t CHANNEL_SHAPE[] = {
    SHAPE_RADIAL, SHAPE_BARS, SHAPE_DIAGONAL, SHAPE_CROSS,
    SHAPE_DIAMONDS, SHAPE_TUNNEL, SHAPE_SPIRAL
};
static constexpr uint8_t CHANNEL_SHAPE_COUNT =
    sizeof(CHANNEL_SHAPE) / sizeof(CHANNEL_SHAPE[0]);

// The look of channel `ch` drawn in colour `a`. Shape and oscillation still come
// from the channel number, so two ropes sharing a colour stay distinguishable
// by their movement.
inline PulleysCulture culture_for(CRGB a, uint8_t ch) {
    // A channel reads as ONE colour. colorB is not a contrasting hue any more —
    // it is the same colour a shade off, so the shape shows as relief within a
    // single hue instead of as two colours meeting. The step is deliberately
    // small: ~3% around the wheel (8/256) and ~10% in value, which is enough to
    // see the moving edge from across the space and not enough to read as a
    // second colour.
    //
    // Keep the step in hue+value, never in saturation: lowering saturation
    // lifts all three RGB channels together instead of tinting, and the accent
    // comes out white. The colour is tuned as RGB, so the step is taken in an
    // approximate HSV of it and brought back.
    CHSV hsv = rgb2hsv_approximate(a);
    CRGB b = CHSV((uint8_t)(hsv.hue + 8), hsv.sat, scale8(hsv.val, 230));
    PulleysCulture c;
    c.colorA      = { a.r, a.g, a.b };
    c.colorB      = { b.r, b.g, b.b };
    c.oscillation = 40 + (ch * 11) % 120;          // slow, distinct per channel
    c.shape       = CHANNEL_SHAPE[ch % CHANNEL_SHAPE_COUNT];
    return c;
}

inline PulleysCulture channel_culture(uint8_t ch) {
    return culture_for(channel_color(ch), ch);
}

// Configure an 8×8 slot to render a channel's pattern.
//   serpentine — true for the Screen's matrix, false for the Sensor's panel.
inline void channel_slot_init(PatternSlot& slot, uint8_t ch, CRGB* buffer,
                              uint8_t rows, uint8_t cols, bool serpentine) {
    slot.buffer     = buffer;
    slot.serpentine = serpentine;
    slot.maxBri     = 255;                         // brightness applied by caller
    slot.init(PATTERN_SHAPE, rows, cols);
    slot.culture    = channel_culture(ch);         // set after init — init resets state

    // init() randomizes the starting angle for visual variety; a channel needs
    // the opposite, so every device showing it lands on the same phase for a
    // given mesh clock. Feed pattern_slot_update mesh_now_secs() to get that.
    slot.shapeState.phaseOffset = 0.0f;
}

}  // namespace pulleys
