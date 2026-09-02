#pragma once

#include <stdint.h>
#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_patterns.h>
#include <pulleys_install.h>   // channel_for_device / channel_from_id live here now

// ── pulleys_channel — the visual identity of a channel ────────────────────────
//
// A channel's look is derived from its number alone, with no shared state and
// no negotiation, so a Sensor on the rope and that channel's block on a Screen
// render the identical pattern. That match is the point: it's how someone sees
// which block on the array belongs to the thing they just pulled.
//
// Both roles MUST get their colors and shape from here — duplicating the
// derivation is how the two ends silently drift apart.

namespace pulleys {

static constexpr uint8_t CHANNEL_COUNT = 16;

// Channel hues, hand-picked instead of spread evenly around the wheel. The
// deep-blue band (hue ~150–200) is left out on purpose: at night, in the dark,
// it is the band eyes focus worst, and a blue rope block reads as a smear
// rather than a shape. The first five hues are the ones a small install
// actually uses, so they are the five furthest apart and the easiest to name
// out loud ("the orange one"); the rest fill the gaps for larger channel
// counts and are correspondingly less distinct.
static constexpr uint8_t CHANNEL_HUE[CHANNEL_COUNT] = {
      0,  28,  60,  96, 224,        // red, orange, yellow, green, magenta
     12,  40,  74, 110, 136,
    208, 216, 236, 248,  20,  86
};

inline uint8_t channel_hue(uint8_t ch) { return CHANNEL_HUE[ch % CHANNEL_COUNT]; }

inline CRGB channel_color(uint8_t ch) {
    return CHSV(channel_hue(ch), 235, 255);
}

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

inline PulleysCulture channel_culture(uint8_t ch) {
    uint8_t hue = channel_hue(ch);
    // colorB is a highlight by AREA, not by tint: the renderer already keeps it
    // to a minority of the lit pixels (accentLo in pulleys_patterns.h gives it
    // about a fifth of the form), so the colour itself does not also need to be
    // pale to read as subordinate. It should be a contrasting hue, fully
    // saturated, and only slightly dimmer than the body.
    //
    // Desaturating it was the old mistake: CHSV holds value at 255, so lowering
    // saturation lifts all three RGB channels together instead of tinting, and
    // the accent came out white. Keep saturation at full and carry the contrast
    // in the hue step; carry the subordination in value and in area.
    CRGB a = CHSV(hue, 235, 255);
    CRGB b = CHSV((uint8_t)(hue + 85), 255, 205);
    PulleysCulture c;
    c.colorA      = { a.r, a.g, a.b };
    c.colorB      = { b.r, b.g, b.b };
    c.oscillation = 40 + (ch * 11) % 120;          // slow, distinct per channel
    c.shape       = CHANNEL_SHAPE[ch % CHANNEL_SHAPE_COUNT];
    return c;
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
