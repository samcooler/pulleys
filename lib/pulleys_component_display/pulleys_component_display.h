#pragma once

#include <stdint.h>

// ── pulleys_component_display — the maths behind the component bars ─────────
//
// A component state display draws one bar per game component on an 8×32
// matrix: eight vertical bars side by side along the long axis, each 3 pixels
// wide with a 1-pixel gap, filling upward over the matrix's 8-pixel height. A
// component that has just been lit shows a full bar, and the bar drains away
// until the component goes dark with the bar empty.
//
// The arbiter decides when a component is lit and for how long, and publishes
// each component's charge (MeshComponents). This file is the receiving end:
// given the last packet and the time since, how full is each bar? It is kept
// free of FastLED and the radio so the arithmetic can be tested on a desk.
//
//   pulleys::ComponentDisplayModel m;
//   pulleys::component_display_update(m, pkt.charge, pkt.decay, millis());   // on a packet
//   pulleys::component_display_step(m, millis(), dt);                        // every frame
//   m.shown[bar]                                                // 0..1, what to draw
//
// Coordinates are logical: x runs 0..31 left to right along the long axis, y
// runs 0..7 from the bottom of a bar to its top. component_display_index() turns
// that into an LED, and its flags flip either direction for a panel mounted the
// other way round.

namespace pulleys {

static constexpr uint8_t  COMPONENT_DISPLAY_BARS    = 8;    // one per component: majors 1-5, finals 1-3
static constexpr uint8_t  COMPONENT_DISPLAY_WIDTH   = 32;   // the matrix's long axis, left to right
static constexpr uint8_t  COMPONENT_DISPLAY_HEIGHT  = 8;    // its short axis: a full bar's height
static constexpr uint8_t  COMPONENT_DISPLAY_BAR_W   = 3;    // pixels across one bar
static constexpr uint8_t  COMPONENT_DISPLAY_BAR_GAP = 1;    // dark pixels between bars
static_assert(COMPONENT_DISPLAY_BARS * COMPONENT_DISPLAY_BAR_W +
              (COMPONENT_DISPLAY_BARS - 1) * COMPONENT_DISPLAY_BAR_GAP <= COMPONENT_DISPLAY_WIDTH,
              "the bars have to fit along the matrix");
static constexpr uint32_t COMPONENT_DISPLAY_STALE_MS = 4000;   // no packet for this long: arbiter is gone

// How fast a bar may rise, in bars per second. A pull should read as the bar
// filling rather than appearing, but quickly: 4 is a full bar in a quarter second.
static constexpr float COMPONENT_DISPLAY_FILL_PER_S = 4.0f;

struct ComponentDisplayModel {
    uint8_t  charge[COMPONENT_DISPLAY_BARS] = {};   // from the last packet, percent: 100 = just lit
    uint32_t stampMs  = 0;              // when that packet arrived
    uint16_t decay    = 6667;           // drain rate, thousandths of a percent per second
    bool     heard    = false;          // a packet has ever arrived
    float    shown[COMPONENT_DISPLAY_BARS] = {};    // what is drawn, 0..1
};

inline void component_display_update(ComponentDisplayModel& m, const uint8_t charge[COMPONENT_DISPLAY_BARS],
                         uint16_t decay, uint32_t now) {
    for (uint8_t i = 0; i < COMPONENT_DISPLAY_BARS; i++) m.charge[i] = charge[i];
    m.decay   = decay;
    m.stampMs = now;
    m.heard   = true;
}

// How full a bar should be right now: the last published charge, drained by the
// time since at the published rate. Keeps counting down between packets, and is corrected by the next.
inline float component_display_target(const ComponentDisplayModel& m, uint8_t bar, uint32_t now) {
    float percent = m.charge[bar] - (float)(now - m.stampMs) * m.decay / 1.0e6f;
    float level = percent / 100.0f;
    return level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
}

// Nothing heard for a while, or ever. The bars keep draining on their own, but
// the display should say the arbiter is not talking.
inline bool component_display_stale(const ComponentDisplayModel& m, uint32_t now) {
    return !m.heard || (now - m.stampMs) > COMPONENT_DISPLAY_STALE_MS;
}

// Move what is drawn toward what it should be: up at most COMPONENT_DISPLAY_FILL_PER_S, down
// as fast as the drain says. dt in seconds.
inline void component_display_step(ComponentDisplayModel& m, uint32_t now, float dt) {
    for (uint8_t i = 0; i < COMPONENT_DISPLAY_BARS; i++) {
        float t = component_display_target(m, i, now);
        if (t > m.shown[i]) {
            float up = m.shown[i] + COMPONENT_DISPLAY_FILL_PER_S * dt;
            m.shown[i] = up < t ? up : t;
        } else {
            m.shown[i] = t;
        }
    }
}

// A bar's level as whole pixels plus the brightness (0..255) of the one pixel
// at its top, so a slow drain moves smoothly instead of in 8 jumps.
inline void component_display_pixels(float level, uint8_t& full, uint8_t& edge) {
    float px = level * COMPONENT_DISPLAY_HEIGHT;
    if (px < 0.0f) px = 0.0f;
    if (px > COMPONENT_DISPLAY_HEIGHT) px = COMPONENT_DISPLAY_HEIGHT;
    full = (uint8_t)px;
    edge = (uint8_t)((px - full) * 255.0f);
}

// The leftmost column of a bar.
inline uint8_t component_display_bar_x(uint8_t bar) {
    return bar * (COMPONENT_DISPLAY_BAR_W + COMPONENT_DISPLAY_BAR_GAP);
}

// LED index for logical (x, y). The matrix is wired in serpentine columns along
// its long axis (like the screen): the strip goes up one column of 8, then down
// the next. The flags flip left-right and bottom-top, for a panel mounted the
// other way round.
inline uint16_t component_display_index(uint8_t x, uint8_t y, bool reverseX, bool reverseY) {
    if (reverseX) x = COMPONENT_DISPLAY_WIDTH  - 1 - x;
    if (reverseY) y = COMPONENT_DISPLAY_HEIGHT - 1 - y;
    uint8_t physRow = (x & 1) ? (COMPONENT_DISPLAY_HEIGHT - 1 - y) : y;
    return (uint16_t)x * COMPONENT_DISPLAY_HEIGHT + physRow;
}

}  // namespace pulleys
