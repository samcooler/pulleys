#pragma once

#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <pulleys_identity.h>
#include <math.h>

// ── LED pattern renderer driven by a PulleysCulture ───────────────────────────
// Oscillates between colorA and colorB at the culture's frequency.
// Phase offset per pixel creates a traveling wave effect.

namespace pulleys {

class PatternRenderer {
public:
    static constexpr uint16_t MAX_LEDS = 256;

    void init(CRGB* leds, uint16_t numLeds, uint8_t maxBrightness = 50) {
        _leds = leds;
        _numLeds = (numLeds > MAX_LEDS) ? MAX_LEDS : numLeds;
        _maxBri = maxBrightness;
        _lastMs = millis();
        memset(_sparkle, 0, sizeof(_sparkle));

        // Seed both RNGs from device ID so each board diverges immediately
        uint16_t id = pulleys::identity_id();
        random16_set_seed(id);
        random16_add_entropy(id);
        _rippleSpeed = ((random8() / 255.0f) - 0.5f) * 10.0f;
        _spatialFreqMul  = 0.3f + (random8() / 255.0f) * 0.7f;  // 0.3-1.0x
        _ripplePhase = (random8() / 255.0f) * 6.2832f;
        float midX = (_cols - 1) * 0.5f;
        float midY = (_rows - 1) * 0.5f;
        _cxTarget = midX + ((random8() / 255.0f) - 0.5f) * 4.0f;
        _cyTarget = midY + ((random8() / 255.0f) - 0.5f) * 4.0f;
        _cx = _cxTarget;
        _cy = _cyTarget;
    }

    void setCulture(const PulleysCulture& culture) {
        _culture = culture;
        _spatialFreqBase = 2.0f;  // base spatial frequency for radial ripple
    }

    // Sparse sparkle density: 0.0 = all off, 1.0 = all on
    void setDensity(float density) {
        _density = density;
    }

    // Set matrix dimensions for radial ripple (rows x cols). Default 8x8.
    void setMatrixSize(uint8_t rows, uint8_t cols, bool serpentine = false) {
        _rows = rows;
        _cols = cols;
        _serpentine = serpentine;
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
        _phase += hz * 2.0f * 2.0f * (float)M_PI * dt;
        if (_phase > 6.2832f) _phase -= 6.2832f;

        // Smoothed random walk for ripple parameters
        _rippleSpeed  += _randWalk(dt, 15.0f, 0.1f, _rippleSpeed);

        // Spatial frequency: base from culture, multiplier wanders for longer wavelengths
        _spatialFreqMul += _randWalk(dt, 0.5f, 0.2f, _spatialFreqMul - 0.65f);
        _spatialFreqMul  = _clamp(_spatialFreqMul, 0.3f, 1.0f);
        _rippleFreq      = _spatialFreqBase * _spatialFreqMul;

        _ripplePhase  += _rippleSpeed * dt;
        if (_ripplePhase >  100.0f) _ripplePhase -= 100.0f;
        if (_ripplePhase < -100.0f) _ripplePhase += 100.0f;

        // Wandering center: larger excursion, weaker spring, can roam past edges
        float midX = (_cols - 1) * 0.5f;
        float midY = (_rows - 1) * 0.5f;
        _cxTarget += _randWalk(dt, 4.0f, 0.015f, _cxTarget - midX);
        _cyTarget += _randWalk(dt, 4.0f, 0.015f, _cyTarget - midY);
        _cxTarget = _clamp(_cxTarget, -2.0f, (float)(_cols + 1));
        _cyTarget = _clamp(_cyTarget, -2.0f, (float)(_rows + 1));
        // Smooth follow (EMA)
        float alpha = 1.0f - expf(-0.4f * dt);  // slower follow for smoother wander
        _cx += (_cxTarget - _cx) * alpha;
        _cy += (_cyTarget - _cy) * alpha;

        // Debug: print ripple state every 1s
        if (nowMs - _lastDebugMs >= 1000) {
            _lastDebugMs = nowMs;
            Serial.printf("  [PAT] spd=%.3f freq=%.2f phase=%.1f cx=%.1f cy=%.1f\n",
                          _rippleSpeed, _rippleFreq, _ripplePhase, _cx, _cy);
        }

        CRGB cA = CRGB(_culture.colorA.r, _culture.colorA.g, _culture.colorA.b);
        CRGB cB = CRGB(_culture.colorB.r, _culture.colorB.g, _culture.colorB.b);

        // Last row reserved for culture status display
        uint16_t patternLeds = _numLeds - _cols;

        for (uint16_t i = 0; i < patternLeds; i++) {
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
            if (_serpentine && (row & 1)) col = (_cols - 1) - col;  // unsnake odd rows
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
                if (_sparkle[i] == 0 && random8() < 2) {
                    _sparkle[i] = 255;  // max bright first frame
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

        // ── Status row (last row): colorA(3) | colorB(3) | freq pulse(2) ──
        {
            uint16_t base_i = patternLeds;  // first LED of last row
            CRGB sA = cA; sA.nscale8(_maxBri / 2);
            CRGB sB = cB; sB.nscale8(_maxBri / 2);
            // 3 LEDs color A
            _leds[base_i + 0] = sA;
            _leds[base_i + 1] = sA;
            _leds[base_i + 2] = sA;
            // 3 LEDs color B
            _leds[base_i + 3] = sB;
            _leds[base_i + 4] = sB;
            _leds[base_i + 5] = sB;
            // 2 LEDs pulsing white at culture oscillation frequency
            float pulse = (sinf(_phase) + 1.0f) * 0.5f;  // 0-1
            uint8_t pBri = (uint8_t)(pulse * _maxBri / 2);
            _leds[base_i + 6] = CRGB(pBri, pBri, pBri);
            _leds[base_i + 7] = CRGB(pBri, pBri, pBri);
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
    bool           _serpentine = false;
    uint8_t        _sparkle[MAX_LEDS] = {};
    PulleysCulture _culture = {};

    // Smoothed random walk state
    uint32_t _lastMs      = 0;
    uint32_t _lastDebugMs = 0;
    float    _phase       = 0.0f;
    float    _rippleSpeed = 0.0f;
    float    _rippleFreq      = 1.8f;
    float    _ripplePhase     = 0.0f;
    float    _spatialFreqBase = 1.8f;
    float    _spatialFreqMul  = 1.0f;
    float    _cx              = 3.5f;
    float    _cy              = 3.5f;
    float    _cxTarget        = 3.5f;
    float    _cyTarget        = 3.5f;

    // Brownian walk step: driftRate = random nudge strength,
    // springK = how strongly it pulls back toward center (lower = wider roam).
    static float _randWalk(float dt, float driftRate, float springK, float offset) {
        float nudge = ((random8() / 255.0f) - 0.5f) * 2.0f * driftRate * dt;
        float spring = -offset * springK * dt;
        return nudge + spring;
    }

    static float _clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

} // namespace pulleys
