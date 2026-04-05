#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <Arduino.h>
#include <pulleys_protocol.h>

// ── Culture: the living pattern a device carries ──────────────────────────────
// Two colors + oscillation frequency. Cultures blend when devices meet.

namespace pulleys {

// Map oscillation byte (1–255) to Hz (~0.1 to 5.0 Hz)
inline float culture_osc_to_hz(uint8_t osc) {
    if (osc == 0) osc = 1;
    return 0.02f + (osc / 255.0f) * 0.48f;  // 0.02–0.5 Hz (2–50 sec cycle)
}

// Map Hz back to oscillation byte
inline uint8_t culture_hz_to_osc(float hz) {
    float norm = (hz - 0.02f) / 0.48f;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return (uint8_t)(norm * 255.0f);
}

// Convert HSV (h=0–360, s=0–255, v=0–255) to RGB
static inline PulleysColor _hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
    PulleysColor c;
    if (s == 0) { c.r = c.g = c.b = v; return c; }
    uint8_t region = (h / 60) % 6;
    uint8_t rem = (h % 60) * 255 / 60;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0: c.r = v; c.g = t; c.b = p; break;
        case 1: c.r = q; c.g = v; c.b = p; break;
        case 2: c.r = p; c.g = v; c.b = t; break;
        case 3: c.r = p; c.g = q; c.b = v; break;
        case 4: c.r = t; c.g = p; c.b = v; break;
        default: c.r = v; c.g = p; c.b = q; break;
    }
    return c;
}

// Generate a random culture with vivid, saturated colors
inline PulleysCulture culture_random() {
    PulleysCulture c;
    // Two random hues at least 72 degrees apart (20% of wheel)
    uint16_t hueA = random(0, 360);
    uint16_t offset = random(72, 289);  // 72..288 degrees away
    uint16_t hueB = (hueA + offset) % 360;
    c.colorA = _hsv_to_rgb(hueA, random(200, 256), 255);
    c.colorB = _hsv_to_rgb(hueB, random(200, 256), 255);
    c.oscillation = random(30, 226);  // ~0.7–4.4 Hz, avoiding extremes
    return c;
}

// Lerp a single byte
static inline uint8_t _lerp8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)(a + (b - a) * t);
}

// Blend two cultures. ratio=0.0 returns a, ratio=1.0 returns b.
inline PulleysCulture culture_blend(const PulleysCulture& a, const PulleysCulture& b, float ratio) {
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    PulleysCulture out;
    out.colorA.r    = _lerp8(a.colorA.r, b.colorA.r, ratio);
    out.colorA.g    = _lerp8(a.colorA.g, b.colorA.g, ratio);
    out.colorA.b    = _lerp8(a.colorA.b, b.colorA.b, ratio);
    out.colorB.r    = _lerp8(a.colorB.r, b.colorB.r, ratio);
    out.colorB.g    = _lerp8(a.colorB.g, b.colorB.g, ratio);
    out.colorB.b    = _lerp8(a.colorB.b, b.colorB.b, ratio);
    out.oscillation = _lerp8(a.oscillation, b.oscillation, ratio);
    return out;
}

// Print culture to Serial for debugging
inline void culture_print(const char* label, const PulleysCulture& c) {
    Serial.printf("  %s: A=(%3d,%3d,%3d) B=(%3d,%3d,%3d) osc=%d (%.1fHz)\n",
                  label,
                  c.colorA.r, c.colorA.g, c.colorA.b,
                  c.colorB.r, c.colorB.g, c.colorB.b,
                  c.oscillation, culture_osc_to_hz(c.oscillation));
}

// ── Stubs for future features ─────────────────────────────────────────────────

// TODO: Genetic mutation — nudge colors/frequency randomly within a range
// inline PulleysCulture culture_mutate(const PulleysCulture& c, float intensity) { ... }

// TODO: Ritual check — IMU-driven culture interaction modifier
// inline float culture_ritual_check(const float* accelXYZ) { return 0.0f; }

} // namespace pulleys
