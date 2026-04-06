#pragma once

#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <math.h>

// ── LED pattern renderer driven by a PulleysCulture ───────────────────────────
// Oscillates between colorA and colorB at the culture's frequency.
// Phase offset per pixel creates a traveling wave effect.

namespace pulleys {

class PatternRenderer {
public:
    static constexpr uint16_t MAX_LEDS = 64;

    void init(CRGB* leds, uint16_t numLeds, uint8_t maxBrightness = 50) {
        _leds = leds;
        _numLeds = (numLeds > MAX_LEDS) ? MAX_LEDS : numLeds;
        _maxBri = maxBrightness;
        memset(_sparkle, 0, sizeof(_sparkle));
    }

    void setCulture(const PulleysCulture& culture) {
        _culture = culture;
    }

    // Sparse sparkle density: 0.0 = all off, 1.0 = all on
    void setDensity(float density) {
        _density = density;
    }

    // Set matrix dimensions for radial ripple (rows x cols). Default 8x8.
    void setMatrixSize(uint8_t rows, uint8_t cols) {
        _rows = rows;
        _cols = cols;
    }

    // Call at ~20-30 fps.
    void update() {
        if (!_leds || _numLeds == 0) return;

        float hz = culture_osc_to_hz(_culture.oscillation);
        uint32_t nowMs = millis();
        float dt = (nowMs - _lastMs) / 1000.0f;
        if (_lastMs == 0) dt = 0.033f;  // first frame
        _lastMs = nowMs;

        // Accumulate base phase (wrap to avoid precision loss)
        _phase += hz * 2.0f * (float)M_PI * dt;
        if (_phase > 6.2832f) _phase -= 6.2832f;

        // Smoothed random walk for ripple parameters
        _rippleSpeed  += _randWalk(dt, 0.05f, 0.30f, _rippleSpeed);
        _rippleFreq   += _randWalk(dt, 0.10f, 1.3f,  _rippleFreq - 1.8f);
        _rippleFreq    = _clamp(_rippleFreq, 0.5f, 3.1f);
        _ripplePhase  += _rippleSpeed * dt;
        if (_ripplePhase >  100.0f) _ripplePhase -= 100.0f;
        if (_ripplePhase < -100.0f) _ripplePhase += 100.0f;

        // Wandering center via smoothed random walk
        float midX = (_cols - 1) * 0.5f;
        float midY = (_rows - 1) * 0.5f;
        _cx += _randWalk(dt, 0.3f, _cols * 0.33f, _cx - midX);
        _cy += _randWalk(dt, 0.3f, _rows * 0.33f, _cy - midY);
        _cx = _clamp(_cx, midX - _cols * 0.33f, midX + _cols * 0.33f);
        _cy = _clamp(_cy, midY - _rows * 0.33f, midY + _rows * 0.33f);

        CRGB cA = CRGB(_culture.colorA.r, _culture.colorA.g, _culture.colorA.b);
        CRGB cB = CRGB(_culture.colorB.r, _culture.colorB.g, _culture.colorB.b);

        for (uint16_t i = 0; i < _numLeds; i++) {
            // -- Base pattern: two-color wave --
            float pixelPhase = _phase + (i * 2.0f * (float)M_PI / _numLeds);
            // Sharpen sine: ~15% solid A, ~15% solid B, ~70% smooth transition
            float raw = (sinf(pixelPhase) + 1.0f) * 0.5f;  // 0.0 to 1.0
            float blend;
            if (raw < 0.05f)       blend = 0.0f;
            else if (raw > 0.95f)  blend = 1.0f;
            else                   blend = (raw - 0.05f) / 0.9f;

            CRGB base;
            base.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * blend);
            base.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * blend);
            base.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * blend);
            base.nscale8(_maxBri);

            // -- Radial ripple modulation (stone in water) --
            uint8_t row = i / _cols;
            uint8_t col = i % _cols;
            float dx = col - _cx;
            float dy = row - _cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float ripple = sinf(dist * _rippleFreq - _ripplePhase);
            if (ripple < 0.0f) ripple = 0.0f;  // only positive half
            base.nscale8((uint8_t)(ripple * 255.0f));

            // -- Sparkle overlay --
            if (_density >= 1.0f) {
                _leds[i] = base;
            } else {
                if (_sparkle[i] == 0 && random8() < 3) {
                    _sparkle[i] = 180;  // reduced sparkle peak
                }
                uint8_t bri = _sparkle[i] < 40 ? 40 : _sparkle[i];
                _leds[i] = base;
                _leds[i].nscale8(bri);
                uint8_t decay = (uint8_t)((1.0f - _density) * 20.0f + 4.0f);
                if (_sparkle[i] > decay) {
                    _sparkle[i] -= decay;
                } else {
                    _sparkle[i] = 0;
                }
            }
        }
    }

    // ── Future pattern types ──────────────────────────────────────────────
    // TODO: Matrix-aware patterns for 8x8 traveler grid (radial, spiral, etc.)
    // TODO: Station "breathing" pattern for orb effect
    // TODO: Pattern library system — select pattern by culture traits

private:
    CRGB*          _leds    = nullptr;
    uint16_t       _numLeds = 0;
    uint8_t        _maxBri  = 50;
    float          _density = 1.0f;
    uint8_t        _rows    = 8;
    uint8_t        _cols    = 8;
    uint8_t        _sparkle[MAX_LEDS] = {};
    PulleysCulture _culture = {};

    // Smoothed random walk state
    uint32_t _lastMs      = 0;
    float    _phase       = 0.0f;
    float    _rippleSpeed = 0.0f;
    float    _rippleFreq  = 1.8f;
    float    _ripplePhase = 0.0f;
    float    _cx          = 3.5f;
    float    _cy          = 3.5f;

    // Brownian walk step: drift rate controls how fast it wanders,
    // maxDev is soft spring back toward 0, offset is current displacement.
    static float _randWalk(float dt, float driftRate, float maxDev, float offset) {
        float nudge = ((random8() / 255.0f) - 0.5f) * 2.0f * driftRate * dt;
        // Soft spring: pull back proportional to how far from center
        float spring = -offset * (driftRate / maxDev) * dt;
        return nudge + spring;
    }

    static float _clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

} // namespace pulleys
