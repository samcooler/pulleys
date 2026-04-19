#pragma once

#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_culture.h>
#include <pulleys_identity.h>
#include <math.h>

namespace pulleys {

// ── Wanderer: jerk-impulse physics primitive ──────────────────────────────────
// Smooth random motion: jerk(random) → acceleration → velocity → position.
// Optional bounds with reflection, or unbounded (e.g. for angles).

struct Wanderer {
    float pos = 0.0f;
    float vel = 0.0f;
    float acc = 0.0f;

    float jerkScale  = 5.0f;   // amplitude of random impulses
    float accDecay   = 2.0f;   // acceleration decay rate (higher = faster decay)
    float velDecay   = 0.3f;   // velocity decay rate
    bool  bounded    = true;   // enable reflection at lo/hi
    float lo         = 0.0f;
    float hi         = 7.0f;

    void configure(float p, float js, float ad, float vd, bool bnd, float l, float h) {
        pos = p; vel = 0; acc = 0;
        jerkScale = js; accDecay = ad; velDecay = vd;
        bounded = bnd; lo = l; hi = h;
    }

    void update(float dt) {
        acc += ((random8() / 255.0f) - 0.5f) * 2.0f * jerkScale * dt;
        acc *= expf(-accDecay * dt);
        vel += acc * dt;
        vel *= expf(-velDecay * dt);
        pos += vel * dt;
        if (bounded) {
            if (pos < lo) { pos = lo; vel = fabsf(vel); }
            if (pos > hi) { pos = hi; vel = -fabsf(vel); }
        }
    }

    void initRandom() {
        pos = lo + (random8() / 255.0f) * (hi - lo);
        vel = 0;
        acc = 0;
    }
};

// ── Pattern types ─────────────────────────────────────────────────────────────

enum PatternType : uint8_t {
    PATTERN_RADIAL_RIPPLE = 0,
    PATTERN_PILLOW_SEESAW = 1,
    PATTERN_COUNT
};

// ── RadialRipple state ────────────────────────────────────────────────────────
// Traveling color wave + concentric ripple rings + sparkle overlay.

struct RadialRippleState {
    static constexpr uint16_t MAX_SPARKLE = 256;

    float    phase          = 0.0f;
    float    ripplePhase    = 0.0f;
    float    spatialFreqBase = 2.0f;

    Wanderer rippleSpeed;   // controls ripple animation speed
    Wanderer spatialFreqMul; // wavelength of ripples
    Wanderer cx;            // center X
    Wanderer cy;            // center Y
    Wanderer waveDir;       // wave direction angle (unbounded)

    float    density        = 1.0f;
    uint8_t  sparkle[MAX_SPARKLE] = {};

    bool     useGravity     = false;
    float    gravX          = 0.0f;
    float    gravY          = 0.0f;

    void init(uint8_t rows, uint8_t cols) {
        phase = 0.0f;
        ripplePhase = (random8() / 255.0f) * 6.2832f;
        spatialFreqBase = 2.0f;
        memset(sparkle, 0, sizeof(sparkle));

        rippleSpeed.configure(((random8() / 255.0f) - 0.5f) * 10.0f,
                              10.0f, 2.0f, 0.3f, true, -6.0f, 6.0f);

        spatialFreqMul.configure(0.3f + (random8() / 255.0f) * 0.7f,
                                 1.0f, 2.0f, 0.3f, true, 0.3f, 1.0f);

        cx.configure((cols - 1) * 0.5f,
                     5.0f, 2.0f, 0.3f, true, 1.0f, (float)(cols - 2));

        cy.configure((rows - 1) * 0.5f,
                     5.0f, 2.0f, 0.3f, true, 1.0f, (float)(rows - 2));

        waveDir.configure(0, 1.5f, 2.0f, 0.5f, false, 0, 0);

        useGravity = false;
    }
};

// ── PillowSeesaw state ────────────────────────────────────────────────────────
// Left/right antiphase color seesaw with 2D cosine "pillow" brightness dome.

struct PillowSeesawState {
    static constexpr uint8_t MAX_ROWS = 8;
    static constexpr uint8_t MAX_COLS = 8;

    uint8_t pillowMap[MAX_ROWS][MAX_COLS];
    bool    inited = false;

    void init(uint8_t rows, uint8_t cols) {
        uint8_t r_count = (rows > MAX_ROWS) ? MAX_ROWS : rows;
        uint8_t c_count = (cols > MAX_COLS) ? MAX_COLS : cols;
        float cxMid = (c_count - 1) * 0.5f;
        float cyMid = (r_count - 1) * 0.5f;
        for (uint8_t r = 0; r < r_count; r++) {
            float vy = cosf((r - cyMid) / (cyMid + 0.001f) * (float)M_PI * 0.5f);
            vy = vy * vy;
            for (uint8_t c = 0; c < c_count; c++) {
                float dx = (c - cxMid) / (cxMid + 0.5f);
                float vx = cosf(dx * (float)M_PI * 0.5f);
                vx = vx * vx;
                pillowMap[r][c] = (uint8_t)(vy * vx * 255.0f);
            }
        }
        inited = true;
    }
};

// ── PatternSlot: one culture rendered onto an 8×N region ──────────────────────

struct PatternSlot {
    // Output buffer (points into the LED array)
    CRGB*          buffer     = nullptr;
    uint8_t        rows       = 8;
    uint8_t        cols       = 8;
    bool           serpentine = false;
    uint8_t        maxBri     = 50;

    PulleysCulture culture    = {};
    PatternType    type       = PATTERN_RADIAL_RIPPLE;

    // Pattern-specific state (only one active at a time)
    RadialRippleState  radialRipple;
    PillowSeesawState  pillowSeesaw;

    void init(PatternType patType, uint8_t r, uint8_t c) {
        type = patType;
        rows = r;
        cols = c;
        if (type == PATTERN_RADIAL_RIPPLE) {
            radialRipple.init(rows, cols);
        } else if (type == PATTERN_PILLOW_SEESAW) {
            pillowSeesaw.init(rows, cols);
        }
    }
};

// ── Pattern update functions ──────────────────────────────────────────────────

static inline void _pattern_radial_ripple_update(PatternSlot& slot, float dt) {
    RadialRippleState& s = slot.radialRipple;
    float hz = culture_osc_to_hz(slot.culture.oscillation);

    // Phase accumulation
    s.phase += hz * 2.0f * 1.2f * (float)M_PI * dt;
    if (s.phase > 6.2832f) s.phase -= 6.2832f;

    // Update wanderers
    s.rippleSpeed.update(dt);
    s.spatialFreqMul.update(dt);
    float rippleFreq = s.spatialFreqBase * s.spatialFreqMul.pos;

    s.ripplePhase += s.rippleSpeed.pos * dt;
    if (s.ripplePhase >  100.0f) s.ripplePhase -= 100.0f;
    if (s.ripplePhase < -100.0f) s.ripplePhase += 100.0f;

    // Center: gravity-driven or random wander
    if (s.useGravity) {
        float midX = (slot.cols - 1) * 0.5f;
        float midY = (slot.rows - 1) * 0.5f;
        float range = 3.0f;
        float targetX = midX - s.gravY * range;
        float targetY = midY + s.gravX * range;
        if (targetX < s.cx.lo) targetX = s.cx.lo;
        if (targetX > s.cx.hi) targetX = s.cx.hi;
        if (targetY < s.cy.lo) targetY = s.cy.lo;
        if (targetY > s.cy.hi) targetY = s.cy.hi;
        float followRate = 1.0f - expf(-4.0f * dt);
        s.cx.pos += (targetX - s.cx.pos) * followRate;
        s.cy.pos += (targetY - s.cy.pos) * followRate;
    } else {
        s.cx.update(dt);
        s.cy.update(dt);
    }

    // Wave direction
    s.waveDir.update(dt);
    float waveDx = cosf(s.waveDir.pos);
    float waveDy = sinf(s.waveDir.pos);

    CRGB cA(slot.culture.colorA.r, slot.culture.colorA.g, slot.culture.colorA.b);
    CRGB cB(slot.culture.colorB.r, slot.culture.colorB.g, slot.culture.colorB.b);

    uint16_t numPx = (uint16_t)slot.rows * slot.cols;
    for (uint16_t i = 0; i < numPx; i++) {
        uint8_t row = i / slot.cols;
        uint8_t col = i % slot.cols;
        if (slot.serpentine && (row & 1)) col = (slot.cols - 1) - col;

        // Two-color wave
        float proj = col * waveDx + row * waveDy;
        float pixelPhase = s.phase + proj * 2.0f * (float)M_PI / (float)slot.cols;
        float raw = (sinf(pixelPhase) + 1.0f) * 0.5f;
        float blend;
        if (raw < 0.05f)       blend = 0.0f;
        else if (raw > 0.95f)  blend = 1.0f;
        else                   blend = (raw - 0.05f) / 0.9f;

        CRGB base;
        base.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * blend);
        base.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * blend);
        base.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * blend);
        base.nscale8(slot.maxBri);

        // Radial ripple modulation
        float dx = col - s.cx.pos;
        float dy = row - s.cy.pos;
        float dist = sqrtf(dx * dx + dy * dy);
        float ripple = sinf(dist * rippleFreq - s.ripplePhase);
        if (ripple < 0.0f) ripple = 0.0f;
        base.nscale8((uint8_t)(ripple * 255.0f));

        // Sparkle overlay
        if (s.density >= 1.0f) {
            slot.buffer[i] = base;
        } else {
            if (s.sparkle[i] == 0 && random8() < 2) {
                s.sparkle[i] = 255;
            }
            uint8_t bri = s.sparkle[i] < 40 ? 40 : s.sparkle[i];
            slot.buffer[i] = base;
            slot.buffer[i].nscale8(bri);
            uint8_t decay = (uint8_t)((1.0f - s.density) * 20.0f + 4.0f);
            if (s.sparkle[i] > decay) {
                s.sparkle[i] -= decay;
            } else {
                s.sparkle[i] = 0;
            }
        }
    }
}

static inline void _pattern_pillow_seesaw_update(PatternSlot& slot, float dt, float t) {
    PillowSeesawState& s = slot.pillowSeesaw;
    if (!s.inited) s.init(slot.rows, slot.cols);

    float hz = culture_osc_to_hz(slot.culture.oscillation);
    float wave = (sinf(t * hz * 2.0f * (float)M_PI) + 1.0f) * 0.5f;
    float waveInv = 1.0f - wave;

    CRGB cA(slot.culture.colorA.r, slot.culture.colorA.g, slot.culture.colorA.b);
    CRGB cB(slot.culture.colorB.r, slot.culture.colorB.g, slot.culture.colorB.b);

    // Left half: A→B, Right half: B→A (antiphase)
    CRGB leftC;
    leftC.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * wave);
    leftC.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * wave);
    leftC.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * wave);

    CRGB rightC;
    rightC.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * waveInv);
    rightC.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * waveInv);
    rightC.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * waveInv);

    for (uint8_t r = 0; r < slot.rows; r++) {
        for (uint8_t c = 0; c < slot.cols; c++) {
            uint8_t col = c;
            if (slot.serpentine && (r & 1)) col = (slot.cols - 1) - c;
            uint16_t idx = (uint16_t)r * slot.cols + col;

            uint8_t pillow = scale8(s.pillowMap[r][c], slot.maxBri);
            slot.buffer[idx] = (c < slot.cols / 2) ? leftC : rightC;
            slot.buffer[idx].nscale8(pillow);
        }
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

static inline void pattern_slot_update(PatternSlot& slot, float dt, float t) {
    if (!slot.buffer) return;
    switch (slot.type) {
        case PATTERN_RADIAL_RIPPLE:
            _pattern_radial_ripple_update(slot, dt);
            break;
        case PATTERN_PILLOW_SEESAW:
            _pattern_pillow_seesaw_update(slot, dt, t);
            break;
        default:
            break;
    }
}

// ── PatternRenderer: high-level wrapper for traveler ──────────────────────────
// Manages one PatternSlot + optional status row + timing.

class PatternRenderer {
public:
    static constexpr uint16_t MAX_LEDS = 256;

    void init(CRGB* leds, uint16_t numLeds, uint8_t maxBrightness = 50) {
        _leds = leds;
        _numLeds = (numLeds > MAX_LEDS) ? MAX_LEDS : numLeds;
        _lastMs = millis();

        // Seed both RNGs from device ID so each board diverges immediately
        uint16_t id = pulleys::identity_id();
        random16_set_seed(id);
        random16_add_entropy(id);

        _slot.buffer = leds;
        _slot.maxBri = maxBrightness;
        _slot.init(_patternType, _slot.rows, _slot.cols);
    }

    void setCulture(const PulleysCulture& culture) {
        _slot.culture = culture;
    }

    void setDensity(float density) {
        _slot.radialRipple.density = density;
    }

    void setMatrixSize(uint8_t rows, uint8_t cols, bool serpentine = false) {
        _slot.rows = rows;
        _slot.cols = cols;
        _slot.serpentine = serpentine;
    }

    void setPatternType(PatternType type) {
        if (type == _patternType) return;
        _patternType = type;
        _slot.init(type, _slot.rows, _slot.cols);
    }

    PatternType getPatternType() const { return _patternType; }

    void update() {
        if (!_leds || _numLeds == 0) return;

        uint32_t nowMs = millis();
        float dt = (nowMs - _lastMs) / 1000.0f;
        if (_lastMs == 0) dt = 0.033f;
        _lastMs = nowMs;
        float t = nowMs / 1000.0f;

        // Render pattern into main grid (excluding status row)
        uint16_t patternLeds = _numLeds - _slot.cols;
        _slot.buffer = _leds;
        // Temporarily set rows to exclude status row
        uint8_t fullRows = _slot.rows;
        _slot.rows = (uint8_t)(patternLeds / _slot.cols);

        pattern_slot_update(_slot, dt, t);

        _slot.rows = fullRows;

        // Debug
        if (nowMs - _lastDebugMs >= 1000) {
            _lastDebugMs = nowMs;
            if (_patternType == PATTERN_RADIAL_RIPPLE) {
                auto& s = _slot.radialRipple;
                Serial.printf("  [PAT] cx=%.2f cy=%.2f vx=%.2f vy=%.2f ax=%.2f ay=%.2f\n",
                              s.cx.pos, s.cy.pos, s.cx.vel, s.cy.vel, s.cx.acc, s.cy.acc);
            }
        }

        // Status row (last row): colorA(3) | colorB(3) | freq pulse(2)
        CRGB cA(_slot.culture.colorA.r, _slot.culture.colorA.g, _slot.culture.colorA.b);
        CRGB cB(_slot.culture.colorB.r, _slot.culture.colorB.g, _slot.culture.colorB.b);
        uint16_t base_i = patternLeds;
        uint8_t maxBri = _slot.maxBri;
        CRGB sA = cA; sA.nscale8(maxBri / 2);
        CRGB sB = cB; sB.nscale8(maxBri / 2);
        _leds[base_i + 0] = sA;
        _leds[base_i + 1] = sA;
        _leds[base_i + 2] = sA;
        _leds[base_i + 3] = sB;
        _leds[base_i + 4] = sB;
        _leds[base_i + 5] = sB;
        // Pulsing white — need phase from the slot
        float phase = 0;
        if (_patternType == PATTERN_RADIAL_RIPPLE) {
            phase = _slot.radialRipple.phase;
        } else {
            phase = t * culture_osc_to_hz(_slot.culture.oscillation) * 2.0f * (float)M_PI;
        }
        float pulse = (sinf(phase) + 1.0f) * 0.5f;
        uint8_t pBri = (uint8_t)(pulse * maxBri / 2);
        _leds[base_i + 6] = CRGB(pBri, pBri, pBri);
        _leds[base_i + 7] = CRGB(pBri, pBri, pBri);
    }

    void setGravity(float ax, float ay) {
        _slot.radialRipple.gravX = ax;
        _slot.radialRipple.gravY = ay;
        _slot.radialRipple.useGravity = true;
    }

private:
    CRGB*          _leds         = nullptr;
    uint16_t       _numLeds      = 0;
    uint32_t       _lastMs       = 0;
    uint32_t       _lastDebugMs  = 0;
    PatternType    _patternType  = PATTERN_RADIAL_RIPPLE;
    PatternSlot    _slot;
};

} // namespace pulleys
