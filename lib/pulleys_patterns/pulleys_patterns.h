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
        _cx = (_cols - 1) * 0.5f;
        _cy = (_rows - 1) * 0.5f;
        _cxVel = 0; _cyVel = 0;
        _cxAcc = 0; _cyAcc = 0;
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

        // Ripple speed: jerk → acc → vel → position (rippleSpeed is the "position")
        _rsAcc += ((random8() / 255.0f) - 0.5f) * 2.0f * 20.0f * dt;
        _rsAcc *= expf(-2.0f * dt);
        _rsVel += _rsAcc * dt;
        _rsVel *= expf(-0.3f * dt);
        _rippleSpeed += _rsVel * dt;
        if (_rippleSpeed < -15.0f) { _rippleSpeed = -15.0f; _rsVel = fabsf(_rsVel); }
        if (_rippleSpeed >  15.0f) { _rippleSpeed =  15.0f; _rsVel = -fabsf(_rsVel); }

        // Spatial frequency multiplier: jerk → acc → vel → position
        _sfAcc += ((random8() / 255.0f) - 0.5f) * 2.0f * 1.0f * dt;
        _sfAcc *= expf(-2.0f * dt);
        _sfVel += _sfAcc * dt;
        _sfVel *= expf(-0.3f * dt);
        _spatialFreqMul += _sfVel * dt;
        if (_spatialFreqMul < 0.3f) { _spatialFreqMul = 0.3f; _sfVel = fabsf(_sfVel); }
        if (_spatialFreqMul > 1.0f) { _spatialFreqMul = 1.0f; _sfVel = -fabsf(_sfVel); }
        _rippleFreq = _spatialFreqBase * _spatialFreqMul;

        _ripplePhase += _rippleSpeed * dt;
        if (_ripplePhase >  100.0f) _ripplePhase -= 100.0f;
        if (_ripplePhase < -100.0f) _ripplePhase += 100.0f;

        // Wandering center: jerk impulses → acceleration → velocity → position
        float jerkScale = 5.0f;
        _cxAcc += ((random8() / 255.0f) - 0.5f) * 2.0f * jerkScale * dt;
        _cyAcc += ((random8() / 255.0f) - 0.5f) * 2.0f * jerkScale * dt;
        float accDecay = expf(-2.0f * dt);   // acceleration half-life ~0.35s
        _cxAcc *= accDecay;
        _cyAcc *= accDecay;
        _cxVel += _cxAcc * dt;
        _cyVel += _cyAcc * dt;
        float velDecay = expf(-0.3f * dt);   // velocity half-life ~2.3s
        _cxVel *= velDecay;
        _cyVel *= velDecay;
        _cx += _cxVel * dt;
        _cy += _cyVel * dt;
        // Reflect off boundaries
        if (_cx < -1.0f) { _cx = -1.0f; _cxVel = fabsf(_cxVel); }
        if (_cx > (float)_cols) { _cx = (float)_cols; _cxVel = -fabsf(_cxVel); }
        if (_cy < -1.0f) { _cy = -1.0f; _cyVel = fabsf(_cyVel); }
        if (_cy > (float)_rows) { _cy = (float)_rows; _cyVel = -fabsf(_cyVel); }

        // Debug: print ripple state every 1s
        if (nowMs - _lastDebugMs >= 1000) {
            _lastDebugMs = nowMs;
            Serial.printf("  [PAT] cx=%.2f cy=%.2f vx=%.2f vy=%.2f ax=%.2f ay=%.2f\n",
                          _cx, _cy, _cxVel, _cyVel, _cxAcc, _cyAcc);
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
    float    _rsVel           = 0.0f;
    float    _rsAcc           = 0.0f;
    float    _rippleFreq      = 1.8f;
    float    _ripplePhase     = 0.0f;
    float    _spatialFreqBase = 1.8f;
    float    _spatialFreqMul  = 1.0f;
    float    _sfVel           = 0.0f;
    float    _sfAcc           = 0.0f;
    float    _cx              = 3.5f;
    float    _cy              = 3.5f;
    float    _cxVel           = 0.0f;
    float    _cyVel           = 0.0f;
    float    _cxAcc           = 0.0f;
    float    _cyAcc           = 0.0f;
};

} // namespace pulleys
