#pragma once

#include <stdint.h>
#include <math.h>
#include <FastLED.h>
#include <pulleys_panel.h>
#include <pulleys_patterns.h>   // Wanderer

// ── pulleys_wave — a bubbling pot of colour on a panel ────────────────────────
//
// Three sine waves summed across the panel, each with its own frequency in
// space and in time, and each of those slowly wandering. Nothing in here
// repeats: the three drift in and out of agreement, so the surface gathers into
// a bright crest, pulls apart, and gathers somewhere else.
//
// Colour is read off the sum and stays in one narrow arc around a centre hue.
// That is the whole trick to it looking like one material rather than a colour
// cycle: the pot bubbles *around* a colour, never away from it.
//
// A second field comes free. Every component is sampled as both sine and
// cosine of the same argument; the sine sum drives hue and the cosine sum
// drives brightness. Being a quarter cycle apart, the bright bubbles sit
// between the colour bubbles instead of on top of them, which is what reads as
// two things intermingling rather than one thing pulsing.
//
// Motion comes from Wanderers (pulleys_patterns): jerk → acceleration →
// velocity, bounded with reflection. A wandering frequency drifts the way a
// physical thing does, where a random frequency per frame would just seethe.
//
// Phase is integrated per frame, not derived from absolute time, because the
// speeds themselves move -- multiplying a changing speed by a running clock
// makes the wave jump every time the speed changes. The cost is that this is
// the one pattern in the piece that is NOT the same on two boards at the same
// moment: each pot bubbles from its own random state. Channel patterns must
// match across devices (see pulleys_channel); a pot has nothing to match.

namespace pulleys {

static constexpr uint8_t WAVE_COMPONENTS = 3;

struct WaveParams {
    // The arc the pot stays inside. 39 is the midpoint of the topaz/amber pair
    // this display started from (hue 52 and 26), and the swing reaches both
    // ends of it, so the colour range is the same one — just no longer walked
    // in order.
    uint8_t centerHue = 39;
    uint8_t hueSwing  = 13;    // ± around centre at full crest
    uint8_t sat       = 250;   // near-full: saturation is what keeps it jewel

    // Brightness range of the bubbling. The floor is well above black -- on a
    // panel two pixels tall a dark patch reads as a dead pixel, not as depth.
    uint8_t valLo     = 165;
    uint8_t valHi     = 255;

    // Bounds the wanderers move within.
    float cyclesLo    = 0.6f;  // periods across one run — under 1 is a swell
    float cyclesHi    = 3.0f;  // over ~3 starts to alias on a 16-pixel run
    float slideMax    = 0.5f;  // runs per second, signed: waves cross each other
    float swingLo     = 0.45f; // how much of hueSwing is in play, wandering
    float swingHi     = 1.0f;

    uint8_t maxBri    = 42;    // matches the screen's MAX_BRIGHTNESS
};

// One of the three waves. Frequencies wander; phase is integrated from them.
struct WaveComponent {
    Wanderer cycles;     // spatial frequency, periods across a run
    Wanderer slide;      // temporal frequency, runs per second, signed
    Wanderer amp;        // weight in the sum, so components take turns leading
    float    phase    = 0.0f;
    float    rowPhase = 0.0f;   // fixed tilt across the rows, per component
};

struct WaveState {
    WaveComponent comp[WAVE_COMPONENTS];
    Wanderer      swing;        // how wide the hue arc is at this moment
};

// Wanderer tuning. Small jerk with heavy velocity decay is the difference
// between a frequency that drifts and one that thrashes; these are slow on
// purpose, so the pot takes tens of seconds to change its mind.
static constexpr float WAVE_JERK      = 0.9f;
static constexpr float WAVE_ACC_DECAY = 2.2f;
static constexpr float WAVE_VEL_DECAY = 1.1f;

inline void wave_init(WaveState& st, const WaveParams& w) {
    // Row tilts are fixed rather than wandering: they are what tells the three
    // components apart across a 2-row panel, and a tilt that drifted would let
    // all three line up flat at the same time.
    static constexpr float ROW_TILT[WAVE_COMPONENTS] = { 0.0f, 0.55f, -0.95f };

    for (uint8_t i = 0; i < WAVE_COMPONENTS; i++) {
        WaveComponent& c = st.comp[i];
        c.cycles.configure(0, WAVE_JERK, WAVE_ACC_DECAY, WAVE_VEL_DECAY,
                           true, w.cyclesLo, w.cyclesHi);
        c.slide.configure(0, WAVE_JERK, WAVE_ACC_DECAY, WAVE_VEL_DECAY,
                          true, -w.slideMax, w.slideMax);
        c.amp.configure(0, WAVE_JERK, WAVE_ACC_DECAY, WAVE_VEL_DECAY,
                        true, 0.35f, 1.0f);
        c.cycles.initRandom();
        c.slide.initRandom();
        c.amp.initRandom();
        c.phase    = (random8() / 255.0f) * (float)TWO_PI;
        c.rowPhase = ROW_TILT[i];
    }
    st.swing.configure(0, WAVE_JERK, WAVE_ACC_DECAY, WAVE_VEL_DECAY,
                       true, w.swingLo, w.swingHi);
    st.swing.initRandom();
}

inline void wave_render(const Panel& p, const WaveParams& w, WaveState& st,
                        float dt) {
    // Advance the motion once per frame, never per pixel: these describe the
    // moment, and a pixel that advanced them would shear the field across x.
    float ampSum = 0.0f;
    for (uint8_t i = 0; i < WAVE_COMPONENTS; i++) {
        WaveComponent& c = st.comp[i];
        c.cycles.update(dt);
        c.slide.update(dt);
        c.amp.update(dt);
        c.phase += (float)TWO_PI * c.slide.pos * dt;
        if (c.phase > (float)TWO_PI)  c.phase -= (float)TWO_PI;
        if (c.phase < 0.0f)           c.phase += (float)TWO_PI;
        ampSum += c.amp.pos;
    }
    st.swing.update(dt);
    if (ampSum < 0.001f) ampSum = 0.001f;
    float norm  = 1.0f / ampSum;
    float swing = (float)w.hueSwing * st.swing.pos;

    for (uint8_t y = 0; y < p.rows; y++) {
        for (uint8_t x = 0; x < p.cols; x++) {
            float fx = (float)x / (float)p.cols;

            float sSum = 0.0f, cSum = 0.0f;
            for (uint8_t i = 0; i < WAVE_COMPONENTS; i++) {
                const WaveComponent& c = st.comp[i];
                float arg = c.phase + (float)TWO_PI * c.cycles.pos * fx
                            + c.rowPhase * (float)y;
                sSum += c.amp.pos * sinf(arg);
                cSum += c.amp.pos * cosf(arg);
            }
            sSum *= norm;   // -1..1
            cSum *= norm;   // -1..1, a quarter cycle out of step with sSum

            uint8_t hue = (uint8_t)((int16_t)w.centerHue
                                    + (int16_t)lroundf(swing * sSum));
            uint8_t val = (uint8_t)(w.valLo
                          + (w.valHi - w.valLo) * (0.5f + 0.5f * cSum));

            CRGB c = CHSV(hue, w.sat, val);
            c.nscale8(w.maxBri);
            panel_set(p, x, y, c);
        }
    }
}

}  // namespace pulleys
