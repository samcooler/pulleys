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

    float jerkScale  = 5.0f;
    float accDecay   = 2.0f;
    float velDecay   = 0.3f;
    bool  bounded    = true;
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

// ── Shape types ───────────────────────────────────────────────────────────────

enum ShapeType : uint8_t {
    SHAPE_RADIAL    = 0,   // concentric rings from wandering center
    SHAPE_BARS      = 1,   // horizontal sine bands scrolling vertically
    SHAPE_DIAGONAL  = 2,   // angled bands at slowly rotating angle
    SHAPE_CHECKER   = 3,   // checkerboard sliding diagonally
    SHAPE_POLKA     = 4,   // two blobs at independent wandering centers
    SHAPE_CROSS     = 5,   // thick plus-sign from wandering center
    SHAPE_DIAMONDS  = 6,   // L1-distance rings (diamond shapes)
    SHAPE_SPIRAL    = 7,   // rotating spiral arms
    SHAPE_TUNNEL    = 8,   // nested squares (Chebyshev distance)
    SHAPE_QUADRANTS = 9,   // four rotating sectors
    SHAPE_COUNT     = 10,
};

inline const char* shape_name(uint8_t shape) {
    static const char* names[SHAPE_COUNT] = {
        "radial", "bars", "diagonal", "checker", "polka",
        "cross", "diamonds", "spiral", "tunnel", "quadrants"
    };
    return shape < SHAPE_COUNT ? names[shape] : "?";
}

// ── Pattern types ─────────────────────────────────────────────────────────────

enum PatternType : uint8_t {
    PATTERN_SHAPE         = 0,   // shape-based — uses culture.shape (traveler)
    PATTERN_PILLOW_SEESAW = 1,   // antiphase seesaw (station)
    PATTERN_COUNT
};

// ── ShapeState: animated state shared across all 10 spatial patterns ──────────
// Wanderers are always updated each frame; unused ones drift harmlessly.

struct ShapeState {
    static constexpr uint8_t MAX_SPARKLE = 64;  // per 8×8 slot

    float phase = 0.0f;

    // Wanderers — which ones drive rendering depends on shape
    Wanderer cx, cy;      // primary center (most shapes) / scroll XY (CHECKER)
    Wanderer cx2, cy2;    // second center (POLKA)
    Wanderer angle;       // rotation angle, unbounded (DIAGONAL, SPIRAL, QUADRANTS)
    Wanderer freq;        // spatial frequency (RADIAL, BARS, DIAGONAL, DIAMONDS, SPIRAL, TUNNEL)
    Wanderer width;       // arm/blob size (CROSS, POLKA)
    Wanderer scroll;      // scroll/zoom offset (BARS, TUNNEL)

    bool  useGravity = false;
    float gravX      = 0.0f;
    float gravY      = 0.0f;

    float density = 1.0f;
    uint8_t sparkle[MAX_SPARKLE] = {};

    void init(uint8_t rows, uint8_t cols) {
        phase = random8() / 255.0f * 6.2832f;
        memset(sparkle, 0, sizeof(sparkle));
        useGravity = false;

        float midX = (cols - 1) * 0.5f;
        float midY = (rows - 1) * 0.5f;

        cx.configure(midX, 5.0f, 2.0f, 0.3f, true, 1.0f, (float)(cols - 2));
        cy.configure(midY, 5.0f, 2.0f, 0.3f, true, 1.0f, (float)(rows - 2));

        // Second center starts offset so two blobs are separated (POLKA)
        float ox = (random8() / 255.0f - 0.5f) * midX;
        float oy = (random8() / 255.0f - 0.5f) * midY;
        cx2.configure(midX + ox, 5.0f, 2.0f, 0.3f, true, 0.5f, (float)(cols - 1));
        cy2.configure(midY + oy, 5.0f, 2.0f, 0.3f, true, 0.5f, (float)(rows - 1));

        angle.configure(random8() / 255.0f * 6.2832f, 0.4f, 2.0f, 0.5f, false, 0, 0);
        freq.configure(0.8f + random8() / 255.0f * 0.8f, 0.8f, 2.0f, 0.3f, true, 0.4f, 2.5f);
        width.configure(2.0f, 1.5f, 2.0f, 0.3f, true, 1.0f, 3.5f);
        scroll.configure(0.0f, 3.0f, 2.0f, 0.3f, false, 0, 0);
    }
};

// ── PillowSeesaw state ────────────────────────────────────────────────────────

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

// ── PatternSlot ───────────────────────────────────────────────────────────────

struct PatternSlot {
    CRGB*          buffer         = nullptr;
    uint8_t        rows           = 8;
    uint8_t        cols           = 8;
    bool           serpentine     = false;
    bool           serpentineFlip = false;
    uint8_t        maxBri         = 50;

    PulleysCulture culture        = {};
    PatternType    type           = PATTERN_SHAPE;

    ShapeState        shapeState;
    PillowSeesawState pillowSeesaw;

    void init(PatternType patType, uint8_t r, uint8_t c) {
        type = patType;
        rows = r;
        cols = c;
        if (type == PATTERN_SHAPE) {
            shapeState.init(rows, cols);
        } else if (type == PATTERN_PILLOW_SEESAW) {
            pillowSeesaw.init(rows, cols);
        }
    }
};

// ── Shape pattern update ──────────────────────────────────────────────────────

static inline void _shape_update(PatternSlot& slot, float dt) {
    ShapeState& s = slot.shapeState;
    uint8_t shape = slot.culture.shape % SHAPE_COUNT;
    float hz = culture_osc_to_hz(slot.culture.oscillation);

    s.phase += hz * 2.0f * (float)M_PI * dt;
    if (s.phase > 6.2832f) s.phase -= 6.2832f;

    // Update center with optional gravity influence
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
    s.cx2.update(dt);
    s.cy2.update(dt);
    s.angle.update(dt);
    s.freq.update(dt);
    s.width.update(dt);
    s.scroll.update(dt);

    CRGB cA(slot.culture.colorA.r, slot.culture.colorA.g, slot.culture.colorA.b);
    CRGB cB(slot.culture.colorB.r, slot.culture.colorB.g, slot.culture.colorB.b);

    uint16_t numPx = (uint16_t)slot.rows * slot.cols;
    for (uint16_t i = 0; i < numPx; i++) {
        uint8_t row = i / slot.cols;
        uint8_t col = i % slot.cols;
        if (slot.serpentine && ((bool)(row & 1) ^ slot.serpentineFlip))
            col = (slot.cols - 1) - col;

        float fc = (float)col;
        float fr = (float)row;

        // Each shape computes a blend [0,1] between colorA and colorB
        float blend = 0.0f;
        switch (shape) {

            case SHAPE_RADIAL: {
                // Concentric rings outward from wandering center
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                float dist = sqrtf(dx*dx + dy*dy);
                blend = (sinf(s.phase + dist * s.freq.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_BARS: {
                // Horizontal sine bands scrolling vertically
                blend = (sinf(s.phase + fr * s.freq.pos + s.scroll.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_DIAGONAL: {
                // Bands at a slowly rotating angle
                float proj = fc * cosf(s.angle.pos) + fr * sinf(s.angle.pos);
                blend = (sinf(s.phase + proj * s.freq.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_CHECKER: {
                // Smooth checkerboard that slides via cx/cy.
                // Product of cosines: ±1 at pixel centers, soft edges between.
                // cv=+1 → (cos(phase)+1)/2, cv=-1 → (-cos(phase)+1)/2 — true alternating.
                float cv = sinf((fc - s.cx.pos + 0.5f) * (float)M_PI)
                         * sinf((fr - s.cy.pos + 0.5f) * (float)M_PI);
                blend = (sinf(s.phase + cv * 0.5f * (float)M_PI) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_POLKA: {
                // Two soft blobs at independent wandering centers.
                // Blob 1 pulls toward colorA, blob 2 toward colorB.
                float d1 = sqrtf((fc-s.cx.pos)*(fc-s.cx.pos) + (fr-s.cy.pos)*(fr-s.cy.pos));
                float d2 = sqrtf((fc-s.cx2.pos)*(fc-s.cx2.pos) + (fr-s.cy2.pos)*(fr-s.cy2.pos));
                float m1 = expf(-d1 / s.width.pos);
                float m2 = expf(-d2 / s.width.pos);
                blend = (sinf(s.phase + (m2 - m1) * (float)M_PI) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_CROSS: {
                // Thick plus-sign from wandering center, arms pulse with phase.
                float dx = fabsf(fc - s.cx.pos);
                float dy = fabsf(fr - s.cy.pos);
                float armMask = fmaxf(1.0f - dx / s.width.pos, 1.0f - dy / s.width.pos);
                if (armMask < 0.0f) armMask = 0.0f;
                blend = (sinf(s.phase + armMask * (float)M_PI) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_DIAMONDS: {
                // L1-distance rings (diamond / rhombus shapes) from wandering center
                float dist_l1 = fabsf(fc - s.cx.pos) + fabsf(fr - s.cy.pos);
                blend = (sinf(s.phase + dist_l1 * s.freq.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_SPIRAL: {
                // Rotating spiral arms — angle wanders for slow drift, phase drives rotation
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                float theta = atan2f(dy, dx) + s.angle.pos;
                float dist = sqrtf(dx*dx + dy*dy);
                blend = (sinf(s.phase + theta + dist * s.freq.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_TUNNEL: {
                // Chebyshev-distance rings (nested squares) with zoom via scroll
                float dx = fabsf(fc - s.cx.pos), dy = fabsf(fr - s.cy.pos);
                float dist_cheb = fmaxf(dx, dy);
                blend = (sinf(s.phase + dist_cheb * s.freq.pos + s.scroll.pos) + 1.0f) * 0.5f;
                break;
            }

            case SHAPE_QUADRANTS: {
                // Four rotating sectors — phase spins them, angle wanders for slow drift
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                float theta = atan2f(dy, dx) + s.angle.pos;
                blend = (cosf(2.0f * theta + s.phase) + 1.0f) * 0.5f;
                break;
            }
        }

        // Mix colors
        CRGB c;
        c.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * blend);
        c.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * blend);
        c.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * blend);
        c.nscale8(slot.maxBri);

        // Sparkle overlay
        if (s.density < 1.0f) {
            if (s.sparkle[i] == 0 && random8() < 2) s.sparkle[i] = 255;
            uint8_t bri = s.sparkle[i] < 40 ? 40 : s.sparkle[i];
            c.nscale8(bri);
            uint8_t decay = (uint8_t)((1.0f - s.density) * 20.0f + 4.0f);
            if (s.sparkle[i] > decay) s.sparkle[i] -= decay;
            else s.sparkle[i] = 0;
        }

        slot.buffer[i] = c;
    }
}

// ── PillowSeesaw pattern update ───────────────────────────────────────────────

static inline void _pattern_pillow_seesaw_update(PatternSlot& slot, float dt, float t) {
    PillowSeesawState& s = slot.pillowSeesaw;
    if (!s.inited) s.init(slot.rows, slot.cols);

    float hz = culture_osc_to_hz(slot.culture.oscillation);
    float wave = (sinf(t * hz * 2.0f * (float)M_PI) + 1.0f) * 0.5f;
    float waveInv = 1.0f - wave;

    CRGB cA(slot.culture.colorA.r, slot.culture.colorA.g, slot.culture.colorA.b);
    CRGB cB(slot.culture.colorB.r, slot.culture.colorB.g, slot.culture.colorB.b);

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
            if (slot.serpentine && ((bool)(r & 1) ^ slot.serpentineFlip)) col = (slot.cols - 1) - c;
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
        case PATTERN_SHAPE:
            _shape_update(slot, dt);
            break;
        case PATTERN_PILLOW_SEESAW:
            _pattern_pillow_seesaw_update(slot, dt, t);
            break;
        default:
            break;
    }
}

// ── PatternRenderer: high-level wrapper for traveler ─────────────────────────
// Manages one PatternSlot + brightness wanderer + timing.

class PatternRenderer {
public:
    static constexpr uint16_t MAX_LEDS = 256;

    void init(CRGB* leds, uint16_t numLeds, uint8_t maxBrightness = 50) {
        _leds    = leds;
        _numLeds = (numLeds > MAX_LEDS) ? MAX_LEDS : numLeds;
        _lastMs  = millis();

        uint16_t id = pulleys::identity_id();
        random16_set_seed(id);
        random16_add_entropy(id);

        _slot.buffer = leds;
        _slot.maxBri = maxBrightness;
        _slot.init(_patternType, _slot.rows, _slot.cols);

        _briWanderer.configure(0.7f, 0.5f, 2.0f, 0.3f, true, 0.22f, 1.0f);
    }

    void setCulture(const PulleysCulture& culture) { _slot.culture = culture; }
    void setDensity(float density)                 { _slot.shapeState.density = density; }

    void setMatrixSize(uint8_t rows, uint8_t cols, bool serpentine = false, bool serpentineFlip = false) {
        _slot.rows           = rows;
        _slot.cols           = cols;
        _slot.serpentine     = serpentine;
        _slot.serpentineFlip = serpentineFlip;
    }

    void setPatternType(PatternType type) {
        if (type == _patternType) return;
        _patternType = type;
        _slot.init(type, _slot.rows, _slot.cols);
    }

    void setGravity(float ax, float ay) {
        _slot.shapeState.gravX = ax;
        _slot.shapeState.gravY = ay;
    }

    void setUseGravity(bool use) { _slot.shapeState.useGravity = use; }

    PatternType getPatternType() const { return _patternType; }

    void update() {
        if (!_leds || _numLeds == 0) return;

        uint32_t nowMs = millis();
        float dt = (nowMs - _lastMs) / 1000.0f;
        if (_lastMs == 0) dt = 0.033f;
        _lastMs = nowMs;
        float t = nowMs / 1000.0f;

        pattern_slot_update(_slot, dt, t);

        _briWanderer.update(dt);
        uint8_t brightScale = (uint8_t)(_briWanderer.pos * 255.0f);
        for (uint16_t i = 0; i < _numLeds; i++) {
            _leds[i].nscale8(brightScale);
        }

        if (nowMs - _lastDebugMs >= 1000) {
            _lastDebugMs = nowMs;
            auto& s = _slot.shapeState;
            Serial.printf("  [PAT] shape=%s cx=%.2f cy=%.2f freq=%.2f angle=%.2f\n",
                          shape_name(_slot.culture.shape % SHAPE_COUNT),
                          s.cx.pos, s.cy.pos, s.freq.pos, s.angle.pos);
        }
    }

private:
    CRGB*       _leds        = nullptr;
    uint16_t    _numLeds     = 0;
    uint32_t    _lastMs      = 0;
    uint32_t    _lastDebugMs = 0;
    PatternType _patternType = PATTERN_SHAPE;
    PatternSlot _slot;
    Wanderer    _briWanderer;
};

} // namespace pulleys
