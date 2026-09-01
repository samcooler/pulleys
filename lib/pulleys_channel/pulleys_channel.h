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
    // colorB is a highlight, not a second identity: a small hue step and a
    // good deal less saturation, so it lands as a pale glint off the body
    // colour. The renderer keeps it to a minority of the lit pixels.
    CRGB a = CHSV(hue, 235, 255);
    CRGB b = CHSV((uint8_t)(hue + 18), 120, 255);
    PulleysCulture c;
    c.colorA      = { a.r, a.g, a.b };
    c.colorB      = { b.r, b.g, b.b };
    c.oscillation = 40 + (ch * 11) % 120;          // slow, distinct per channel
    c.shape       = CHANNEL_SHAPE[ch % CHANNEL_SHAPE_COUNT];
    return c;
}

// ── Channel assignment: which rope each board is on ───────────────────────────
// The install's wiring map, kept in the repo so that swapping in a spare board
// means flashing it and nothing else — no one has to remember what channel the
// dead one was on. Keyed by the device ID identity_init() derives from the MAC,
// which is the same ID the board prints in its whoami line.
//
// Two ways in, both intended, for two different moments:
//
//   ./flash_all.sh with no arguments — the install. Adds every attached board
//   that is missing at the lowest free channel and reflashes with the result.
//   It does not know or care which rope is which; it exists so that a crate of
//   boards works at all, unattended, without anyone deciding anything.
//
//   Editing this block by hand — the calm moment. Which board drives which
//   rope is a judgement about the physical piece that nothing can infer from a
//   MAC address, so setting it deliberately is the better answer, not a
//   deviation from the tool's. Reflash afterwards.
//
// The two do not fight: assign_channels.py only ever adds IDs it does not
// already find here, so a channel you set by hand survives every later run.
// Keep the marker comments, keep channels within the fallback range, and give
// two boards the same channel only if you mean those ropes to read as one.
struct ChannelAssignment { uint16_t id; uint8_t channel; };

static const ChannelAssignment CHANNEL_ASSIGNMENT[] = {
    // ASSIGNMENTS BEGIN
    { 0xEC52,  1 },   // N-EC52
    // ASSIGNMENTS END
};
static constexpr uint8_t CHANNEL_ASSIGNMENT_COUNT =
    sizeof(CHANNEL_ASSIGNMENT) / sizeof(CHANNEL_ASSIGNMENT[0]);

// Channels an unlisted board may fall back to. Channel 0 is deliberately
// excluded: it is the value a board lands on when something has gone wrong
// (an empty NVS read, a zeroed struct), so keeping it out of normal use means
// a rope reporting on channel 0 is always a fault and never a coincidence.
static constexpr uint8_t CHANNEL_FALLBACK_LO = 1;
static constexpr uint8_t CHANNEL_FALLBACK_HI = 12;

// Fallback for a board the table does not name: derive a channel from its ID.
// Two unlisted boards can still collide — with 12 channels a handful of boards
// collide sooner than intuition suggests — so this is a way to keep an
// unregistered board useful and visibly distinct, not a substitute for the
// table. The ID is already a MAC hash; mixing again decorrelates the low bits
// so that boards from one production batch do not land together.
inline uint8_t channel_from_id(uint16_t id) {
    uint32_t h = (uint32_t)id * 2654435761U;
    h ^= h >> 16;
    uint8_t span = CHANNEL_FALLBACK_HI - CHANNEL_FALLBACK_LO + 1;
    return (uint8_t)(CHANNEL_FALLBACK_LO + (h % span));
}

// The channel this board is assigned, or -1 if it is not in the table.
inline int8_t channel_for_device(uint16_t id) {
    for (uint8_t i = 0; i < CHANNEL_ASSIGNMENT_COUNT; i++)
        if (CHANNEL_ASSIGNMENT[i].id == id)
            return (int8_t)CHANNEL_ASSIGNMENT[i].channel;
    return -1;
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
