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
        float t = millis() / 1000.0f;
        float phase = t * hz * 2.0f * (float)M_PI;

        // Ripple center wanders around the middle 2/3 of the matrix
        float midX = (_cols - 1) * 0.5f;
        float midY = (_rows - 1) * 0.5f;
        float wanderX = _cols * 0.33f;  // wander radius = 1/3 of matrix width
        float wanderY = _rows * 0.33f;
        float cx = midX + wanderX * sinf(t * 0.17f + 0.0f) * cosf(t * 0.11f + 2.3f);
        float cy = midY + wanderY * sinf(t * 0.13f + 1.1f) * cosf(t * 0.09f + 3.7f);

        // Woozy random walk: smoothly vary ripple speed (symmetric around 0)
        float rippleSpeed = 0.0f
            + 0.15f * sinf(t * 0.13f)
            + 0.10f * sinf(t * 0.31f + 1.7f)
            + 0.06f * sinf(t * 0.074f + 4.2f);
        // Hard cap
        if (rippleSpeed > 0.30f) rippleSpeed = 0.30f;
        if (rippleSpeed < -0.30f) rippleSpeed = -0.30f;
        float rippleFreq  = 1.8f
            + 0.8f * sinf(t * 0.19f + 0.5f)
            + 0.5f * sinf(t * 0.41f + 2.9f);   // range ~0.5 to 3.1

        CRGB cA = CRGB(_culture.colorA.r, _culture.colorA.g, _culture.colorA.b);
        CRGB cB = CRGB(_culture.colorB.r, _culture.colorB.g, _culture.colorB.b);

        for (uint16_t i = 0; i < _numLeds; i++) {
            // -- Base pattern: two-color wave --
            float pixelPhase = phase + (i * 2.0f * (float)M_PI / _numLeds);
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
            float dx = col - cx;
            float dy = row - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float ripple = sinf(dist * rippleFreq - t * rippleSpeed);
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
};

} // namespace pulleys
