#pragma once

#include <stdint.h>
#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_patterns.h>

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

// Hue is spread evenly around the wheel: 16 channels × 16 = full 8-bit range.
inline uint8_t channel_hue(uint8_t ch) { return (uint8_t)(ch * 16); }

inline CRGB channel_color(uint8_t ch) {
    return CHSV(channel_hue(ch), 235, 255);
}

inline PulleysCulture channel_culture(uint8_t ch) {
    CRGB a = CHSV(channel_hue(ch), 235, 255);
    CRGB b = CHSV((uint8_t)(channel_hue(ch) + 90), 220, 255);
    PulleysCulture c;
    c.colorA      = { a.r, a.g, a.b };
    c.colorB      = { b.r, b.g, b.b };
    c.oscillation = 40 + (ch * 11) % 120;          // slow, distinct per channel
    c.shape       = ch % SHAPE_COUNT;              // 16 channels over 10 shapes
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
}

}  // namespace pulleys
