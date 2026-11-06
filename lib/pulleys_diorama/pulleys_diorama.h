#pragma once

#include <stdint.h>

// ── pulleys_diorama — a ceiling of failing hospital fluorescents ─────────────
//
// The diorama's light is one LED strip cut, in light, into tubes: a run of lit
// pixels the length of a fluorescent bar (5 in at 60 LEDs/m, so 8), then a
// dark gap between fixtures (2 in, so 3). This file decides how bright each
// tube is from moment to moment; the Screen paints that onto the strip in a
// cold, blue-green white.
//
// A tube is never simply on. Switched on, each waits a moment of its own and
// then strikes: a few stuttering flashes before it catches. Once lit it hums
// with a faint shimmer and, now and then, drops out in a quick ragged burst.
// One tube in the room is the bad one, and does it far more often.
//
// Kept free of FastLED and Arduino, like pulleys_component_display: the timing
// is all here and can be read on a desk. Randomness comes from a seed the
// caller supplies, so two boards (or two boots) do not flicker in step.
//
//   pulleys::DioramaLights d;
//   pulleys::diorama_init(d, tubes, esp_random());
//   pulleys::diorama_switch(d, true);              // the lights go on
//   pulleys::diorama_step(d, dt);                  // every frame
//   d.tube[i].level                                // 0..1, what to draw

namespace pulleys {

static constexpr uint8_t DIORAMA_TUBE_PX   = 8;    // 5 in at 60/m is 7.6
static constexpr uint8_t DIORAMA_GAP_PX    = 3;    // 2 in at 60/m is 3.0
static constexpr uint8_t DIORAMA_PERIOD_PX = DIORAMA_TUBE_PX + DIORAMA_GAP_PX;
static constexpr uint8_t DIORAMA_MAX_TUBES = 40;

// Seconds. A tube's wait before it strikes is spread over STRIKE_SPREAD, so a
// room comes on ragged rather than all at once. Between flicker bursts a good
// tube waits around GOOD_GAP_S, the bad one around BAD_GAP_S.
static constexpr float DIORAMA_STRIKE_SPREAD_S = 1.6f;
static constexpr float DIORAMA_GOOD_GAP_S      = 14.0f;
static constexpr float DIORAMA_BAD_GAP_S       = 1.8f;
static constexpr float DIORAMA_SHIMMER         = 0.04f;   // steady-state brightness noise

enum DioramaPhase : uint8_t {
    TUBE_OFF = 0,
    TUBE_WAIT,      // switched on, not yet striking
    TUBE_STRIKE,    // stuttering on
    TUBE_LIT,       // steady, between bursts
    TUBE_BURST,     // a flicker burst in progress
};

struct DioramaTube {
    uint8_t phase   = TUBE_OFF;
    bool    flashOn = false;   // within STRIKE/BURST: the current half of a stutter
    uint8_t left    = 0;       // stutters still to go in this STRIKE/BURST
    float   timer   = 0.0f;    // seconds until this tube's next change
    float   dip     = 0.0f;    // how low a BURST drops it, 0..1
    bool    bad     = false;
    float   level   = 0.0f;    // what to draw, 0..1
};

struct DioramaLights {
    DioramaTube tube[DIORAMA_MAX_TUBES];
    uint8_t     tubes = 0;
    uint32_t    rng   = 1;
    bool        on    = false;
};

// ── Randomness: xorshift32, seeded by the caller ─────────────────────────────
inline uint32_t _diorama_next(DioramaLights& d) {
    uint32_t x = d.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return d.rng = x;
}
inline float _diorama_rand(DioramaLights& d, float lo, float hi) {
    return lo + (hi - lo) * (float)(_diorama_next(d) >> 8) / 16777216.0f;
}

// How many tubes fit on a strip of `leds`: a tube cut short by the end of the
// strip still counts if any of it is there.
inline uint8_t diorama_tubes_for(uint16_t leds) {
    uint16_t n = (leds + DIORAMA_PERIOD_PX - 1) / DIORAMA_PERIOD_PX;
    return n > DIORAMA_MAX_TUBES ? DIORAMA_MAX_TUBES : (uint8_t)n;
}

inline void diorama_init(DioramaLights& d, uint8_t tubes, uint32_t seed) {
    d = DioramaLights{};
    d.tubes = tubes > DIORAMA_MAX_TUBES ? DIORAMA_MAX_TUBES : tubes;
    d.rng   = seed ? seed : 0x9E3779B9u;
    if (d.tubes) d.tube[_diorama_next(d) % d.tubes].bad = true;
}

// Begin a flicker burst: a handful of quick dropouts.
inline void _diorama_burst(DioramaLights& d, DioramaTube& t) {
    t.phase   = TUBE_BURST;
    t.left    = (uint8_t)(2 + _diorama_next(d) % (t.bad ? 7 : 4));
    t.flashOn = false;
    t.dip     = _diorama_rand(d, 0.0f, 0.45f);
    t.timer   = _diorama_rand(d, 0.02f, 0.09f);
}

inline void _diorama_settle(DioramaLights& d, DioramaTube& t) {
    t.phase = TUBE_LIT;
    t.timer = _diorama_rand(d, 0.3f, 1.7f) * (t.bad ? DIORAMA_BAD_GAP_S : DIORAMA_GOOD_GAP_S);
}

// Lights on or off. On: every tube waits its own moment, then strikes. Off: dark at once.
inline void diorama_switch(DioramaLights& d, bool on) {
    if (on == d.on) return;
    d.on = on;
    for (uint8_t i = 0; i < d.tubes; i++) {
        DioramaTube& t = d.tube[i];
        if (on) {
            t.phase = TUBE_WAIT;
            t.timer = _diorama_rand(d, 0.0f, DIORAMA_STRIKE_SPREAD_S);
        } else {
            t.phase = TUBE_OFF;
        }
        t.level = 0.0f;
    }
}

// Shake every lit tube at once -- what the room does when something jolts it.
inline void diorama_jolt(DioramaLights& d) {
    for (uint8_t i = 0; i < d.tubes; i++)
        if (d.tube[i].phase == TUBE_LIT) _diorama_burst(d, d.tube[i]);
}

inline void diorama_step(DioramaLights& d, float dt) {
    for (uint8_t i = 0; i < d.tubes; i++) {
        DioramaTube& t = d.tube[i];
        if (t.phase == TUBE_OFF) { t.level = 0.0f; continue; }
        t.timer -= dt;
        if (t.timer <= 0.0f) {
            switch (t.phase) {
                case TUBE_WAIT:
                    t.phase   = TUBE_STRIKE;
                    t.left    = (uint8_t)(2 + _diorama_next(d) % 4);
                    t.flashOn = true;
                    t.timer   = _diorama_rand(d, 0.03f, 0.08f);
                    break;
                case TUBE_STRIKE:
                    // on, off, on, off ... and the last "on" is the one that holds
                    if (t.flashOn) {
                        if (t.left-- <= 1) { _diorama_settle(d, t); break; }
                        t.flashOn = false;
                        t.timer   = _diorama_rand(d, 0.06f, 0.35f);
                    } else {
                        t.flashOn = true;
                        t.timer   = _diorama_rand(d, 0.03f, 0.10f);
                    }
                    break;
                case TUBE_LIT:
                    _diorama_burst(d, t);
                    break;
                case TUBE_BURST:
                    if (!t.flashOn) {
                        t.flashOn = true;
                        t.timer   = _diorama_rand(d, 0.02f, 0.12f);
                    } else if (t.left-- <= 1) {
                        _diorama_settle(d, t);
                    } else {
                        t.flashOn = false;
                        t.dip     = _diorama_rand(d, 0.0f, 0.45f);
                        t.timer   = _diorama_rand(d, 0.02f, 0.09f);
                    }
                    break;
            }
        }
        switch (t.phase) {
            case TUBE_WAIT:   t.level = 0.0f; break;
            case TUBE_STRIKE: t.level = t.flashOn ? _diorama_rand(d, 0.5f, 1.0f) : 0.0f; break;
            case TUBE_BURST:  t.level = t.flashOn ? 1.0f : t.dip; break;
            default:          t.level = 1.0f - _diorama_rand(d, 0.0f, DIORAMA_SHIMMER); break;
        }
    }
}

// The first strip pixel of tube `i`.
inline uint16_t diorama_tube_start(uint8_t i) {
    return (uint16_t)i * DIORAMA_PERIOD_PX;
}

}  // namespace pulleys
