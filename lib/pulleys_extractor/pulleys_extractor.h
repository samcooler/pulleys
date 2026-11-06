#pragma once

#include <stdint.h>
#include <math.h>

// ── pulleys_extractor — one colour's score, poured like liquid ───────────────
//
// An extractor is a Screen built for its own LED surface: two 32×8 panels bent
// into a 64×8 ring, wide and horizontal, with a strip hanging straight down
// below it. Five of them, one per game1 colour (everything but purple). Each
// shows how far along its colour is as a column of liquid: the strip fills
// from its far (bottom) end up to the ring, then the ring fills bottom to top.
//
// The liquid is never still. While the surface is in the strip it bobs up and
// down a little; once it reaches the ring it has width, so the surface becomes
// waves -- sines that wrap exactly once round the 64 columns, travelling at
// different speeds and both ways, so it reads as a container sloshing rather
// than a pattern scrolling. A rising level kicks the slosh up, the way pouring
// does, and it settles again over a few seconds.
//
// Everything is in pixel heights measured from the bottom of the strip: strip
// pixel i sits at height i, ring row y at height strip + y. A surface at
// height h lights everything below it fully and the pixel it cuts through by
// the fraction it covers. Kept free of FastLED and the radio, like
// pulleys_component_display, so the maths can be read and tested on a desk.
//
// The chain, in wiring order: panel A (256), panel B (256), then the strip.
// Each panel is wired in serpentine columns of 8 along its long axis, like the
// 8×32 screens; panel B continues the ring where panel A ends. The strip's
// first LED is its top (the end nearest the ring) unless stripReverse.

namespace pulleys {

static constexpr uint8_t  EXTRACTOR_RING_W     = 64;    // columns round the ring
static constexpr uint8_t  EXTRACTOR_RING_H     = 8;     // rows, bottom to top
static constexpr uint8_t  EXTRACTOR_PANEL_W    = 32;    // columns per panel
static constexpr uint16_t EXTRACTOR_RING_LEDS  = (uint16_t)EXTRACTOR_RING_W * EXTRACTOR_RING_H;

// How fast the level may rise, in whole columns per second: a pull should read
// as pouring, not appearing. Falling follows the score at once -- the drain is
// already slow.
static constexpr float EXTRACTOR_POUR_PER_S   = 0.4f;

// The surface's motion, in pixels. Bob is the strip's up-and-down; wave is the
// ring's across-the-width height at rest. Slosh is added on top by pouring and
// decays with SLOSH_HALFLIFE_S; SLOSH_PER_LEVEL is how much a full column's
// worth of rise kicks in, capped at SLOSH_MAX.
static constexpr float EXTRACTOR_BOB_PX         = 0.7f;
static constexpr float EXTRACTOR_WAVE_PX        = 0.9f;
static constexpr float EXTRACTOR_SLOSH_PER_LEVEL = 8.0f;
static constexpr float EXTRACTOR_SLOSH_MAX      = 2.2f;
static constexpr float EXTRACTOR_SLOSH_HALFLIFE_S = 2.5f;

struct ExtractorLiquid {
    float level = 0.0f;   // what is drawn, 0..1 of the whole column
    float slosh = 0.0f;   // extra motion from pouring, pixels
    float t     = 0.0f;   // its own clock, seconds
};

// Move the drawn level toward the score (0..1). dt in seconds.
inline void extractor_step(ExtractorLiquid& l, float target, float dt) {
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    float before = l.level;
    if (target > l.level) {
        float up = l.level + EXTRACTOR_POUR_PER_S * dt;
        l.level = up < target ? up : target;
    } else {
        l.level = target;
    }
    if (l.level > before) l.slosh += (l.level - before) * EXTRACTOR_SLOSH_PER_LEVEL;
    l.slosh *= powf(0.5f, dt / EXTRACTOR_SLOSH_HALFLIFE_S);
    if (l.slosh > EXTRACTOR_SLOSH_MAX) l.slosh = EXTRACTOR_SLOSH_MAX;
    l.t += dt;
}

// 0 at either end of [lo, hi], 1 once `ramp` inside it: motion fades out
// toward empty and full, so an empty column is dark and a full one is flat.
inline float _extractor_taper(float v, float lo, float hi, float ramp) {
    float a = (v - lo) / ramp, b = (hi - v) / ramp;
    float m = a < b ? a : b;
    return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
}

// The level in pixels, from the bottom of the strip.
inline float extractor_height(const ExtractorLiquid& l, uint16_t stripLen) {
    return l.level * (float)(stripLen + EXTRACTOR_RING_H);
}

// The whole surface heaving up and down: two slow sines, out of step.
inline float extractor_bob(const ExtractorLiquid& l, uint16_t stripLen) {
    float h = extractor_height(l, stripLen);
    float amp = (EXTRACTOR_BOB_PX + 0.5f * l.slosh) *
                _extractor_taper(h, 0.0f, (float)(stripLen + EXTRACTOR_RING_H), 1.5f);
    return amp * (0.6f * sinf(l.t * 1.3f) + 0.4f * sinf(l.t * 2.1f + 1.0f));
}

// Where the surface is in the strip. It has no width, so it only bobs.
inline float extractor_strip_surface(const ExtractorLiquid& l, uint16_t stripLen) {
    return extractor_height(l, stripLen) + extractor_bob(l, stripLen);
}

// Where the surface is at ring column x. Waves only once it has climbed into
// the ring, growing over its first two rows and flattening over its last two.
// Each wavenumber is a whole number of cycles round 64, so the seam where the
// ring closes is invisible.
inline float extractor_ring_surface(const ExtractorLiquid& l, uint16_t stripLen, uint8_t x) {
    float h   = extractor_height(l, stripLen);
    float top = (float)(stripLen + EXTRACTOR_RING_H);
    float amp = (EXTRACTOR_WAVE_PX + l.slosh) *
                _extractor_taper(h, (float)stripLen, top, 2.0f);
    float a = 6.2831853f * (float)x / EXTRACTOR_RING_W;
    float w = 0.50f * sinf(1.0f * a + l.t * 0.9f)
            + 0.30f * sinf(2.0f * a - l.t * 1.6f + 2.0f)
            + 0.15f * sinf(3.0f * a + l.t * 2.3f + 4.0f)
            + 0.08f * sinf(5.0f * a - l.t * 3.1f + 1.0f);
    return h + extractor_bob(l, stripLen) + amp * w;
}

// How much of the pixel at height `pixel` is under a surface at `surface`, 0..1.
inline float extractor_cover(float surface, float pixel) {
    float c = surface - pixel;
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

// LED index of ring column x (0..63), row y (0 = bottom).
inline uint16_t extractor_ring_index(uint8_t x, uint8_t y, bool reverseY) {
    if (reverseY) y = EXTRACTOR_RING_H - 1 - y;
    uint8_t panel = x / EXTRACTOR_PANEL_W, col = x % EXTRACTOR_PANEL_W;
    uint8_t physRow = (col & 1) ? (EXTRACTOR_RING_H - 1 - y) : y;
    return (uint16_t)panel * EXTRACTOR_PANEL_W * EXTRACTOR_RING_H +
           (uint16_t)col * EXTRACTOR_RING_H + physRow;
}

// LED index of strip pixel i, counted up from its far (bottom) end.
inline uint16_t extractor_strip_index(uint16_t i, uint16_t stripLen, bool stripReverse) {
    return EXTRACTOR_RING_LEDS + (stripReverse ? i : (uint16_t)(stripLen - 1 - i));
}

}  // namespace pulleys
