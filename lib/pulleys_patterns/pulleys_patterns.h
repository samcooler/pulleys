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

    s.phase += hz * 2.0f * (float)M_PI * dt * 0.5f;
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

    // Per-shape off-zone thresholds for 3-state colorizer
    float offLo, offHi;
    switch (shape) {
        case SHAPE_CHECKER:
            offLo = 0.43f; offHi = 0.57f; break;  // thin dark grid lines
        case SHAPE_RADIAL:
        case SHAPE_DIAMONDS:
            offLo = 0.32f; offHi = 0.68f; break;  // bold dark gaps between rings
        case SHAPE_CROSS:
            offLo = 0.36f; offHi = 0.64f; break;  // dark halo around arm edges
        default:
            offLo = 0.38f; offHi = 0.62f; break;
    }

    // Blend field sampler — accepts fractional (fc, fr) for supersampling
    auto blend_at = [&](float fc, float fr) -> float {
        switch (shape) {
            case SHAPE_RADIAL: {
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                return (sinf(s.phase + sqrtf(dx*dx + dy*dy) * s.freq.pos) + 1.0f) * 0.5f;
            }
            case SHAPE_BARS:
                return (sinf(s.phase + fr * s.freq.pos + s.scroll.pos) + 1.0f) * 0.5f;
            case SHAPE_DIAGONAL: {
                float proj = fc * cosf(s.angle.pos) + fr * sinf(s.angle.pos);
                return (sinf(s.phase + proj * s.freq.pos) + 1.0f) * 0.5f;
            }
            case SHAPE_CHECKER: {
                float cv = sinf((fc - s.cx.pos + 0.5f) * (float)M_PI)
                         * sinf((fr - s.cy.pos + 0.5f) * (float)M_PI);
                return (sinf(s.phase + cv * 0.5f * (float)M_PI) + 1.0f) * 0.5f;
            }
            case SHAPE_POLKA: {
                float d1 = sqrtf((fc-s.cx.pos)*(fc-s.cx.pos)   + (fr-s.cy.pos)*(fr-s.cy.pos));
                float d2 = sqrtf((fc-s.cx2.pos)*(fc-s.cx2.pos) + (fr-s.cy2.pos)*(fr-s.cy2.pos));
                float m1 = expf(-d1 / s.width.pos);
                float m2 = expf(-d2 / s.width.pos);
                return (sinf(s.phase + (m2 - m1) * (float)M_PI) + 1.0f) * 0.5f;
            }
            case SHAPE_CROSS: {
                float dx = fabsf(fc - s.cx.pos), dy = fabsf(fr - s.cy.pos);
                float arm = fmaxf(1.0f - dx / s.width.pos, 1.0f - dy / s.width.pos);
                if (arm < 0.0f) arm = 0.0f;
                return (sinf(s.phase + arm * (float)M_PI) + 1.0f) * 0.5f;
            }
            case SHAPE_DIAMONDS: {
                float dist_l1 = fabsf(fc - s.cx.pos) + fabsf(fr - s.cy.pos);
                return (sinf(s.phase + dist_l1 * s.freq.pos) + 1.0f) * 0.5f;
            }
            case SHAPE_SPIRAL: {
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                float theta = atan2f(dy, dx) + s.angle.pos;
                float dist  = sqrtf(dx*dx + dy*dy);
                return (sinf(s.phase + theta + dist * s.freq.pos) + 1.0f) * 0.5f;
            }
            case SHAPE_TUNNEL: {
                float dx = fabsf(fc - s.cx.pos), dy = fabsf(fr - s.cy.pos);
                return (sinf(s.phase + fmaxf(dx, dy) * s.freq.pos + s.scroll.pos) + 1.0f) * 0.5f;
            }
            case SHAPE_QUADRANTS: {
                float dx = fc - s.cx.pos, dy = fr - s.cy.pos;
                float theta = atan2f(dy, dx) + s.angle.pos;
                return (cosf(2.0f * theta + s.phase) + 1.0f) * 0.5f;
            }
            default: return 0.5f;
        }
    };

    // 3×3 supersample blend field, then soft-threshold the averaged blend once.
    // Averaging blend values (not colors) keeps colorA/colorB pure and black fully black —
    // only the narrow boundary pixels receive a dimmed version of their color.
#ifndef PULLEYS_SSAA
#define PULLEYS_SSAA 3
#endif
    constexpr int   SSAA      = PULLEYS_SSAA;
    constexpr float SSAA_INV  = 1.0f / (SSAA * SSAA);
    constexpr float EDGE_W    = 0.12f;  // half-width of A→off and off→B ramps (in blend units)

    for (uint8_t row = 0; row < slot.rows; row++) {
        for (uint8_t physCol = 0; physCol < slot.cols; physCol++) {
            bool    flipRow = slot.serpentine && (((bool)(row & 1)) ^ slot.serpentineFlip);
            uint8_t linCol  = flipRow ? (slot.cols - 1 - physCol) : physCol;
            uint16_t i      = (uint16_t)row * slot.cols + linCol;

            float blendSum = 0.0f;
            for (int dr = 0; dr < SSAA; dr++) {
                for (int dc = 0; dc < SSAA; dc++) {
                    float sfr = row     + (dr - (SSAA - 1) * 0.5f) / SSAA;
                    float sfc = physCol + (dc - (SSAA - 1) * 0.5f) / SSAA;
                    blendSum += blend_at(sfc, sfr);
                }
            }
            float avgBlend = blendSum * SSAA_INV;

            // Soft ramp at each threshold: pure color in interior, fade to black at edge
            float wA = (offLo - avgBlend) / EDGE_W;
            if (wA < 0.0f) wA = 0.0f; else if (wA > 1.0f) wA = 1.0f;
            float wB = (avgBlend - offHi) / EDGE_W;
            if (wB < 0.0f) wB = 0.0f; else if (wB > 1.0f) wB = 1.0f;

            CRGB c(
                (uint8_t)(wA * cA.r + wB * cB.r + 0.5f),
                (uint8_t)(wA * cA.g + wB * cB.g + 0.5f),
                (uint8_t)(wA * cA.b + wB * cB.b + 0.5f)
            );
            c.nscale8(slot.maxBri);

            // Sparkle overlay (indexed by linear i)
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

        _maxBri  = maxBrightness;
        _slot.buffer = leds;
        _slot.maxBri = 255;  // shape renders full-range colors; brightness gate is briWanderer only
        _slot.init(_patternType, _slot.rows, _slot.cols);

        // Active range [0,1]; gamma 2.5 at render time → mean ~18%, occasional full-bright peaks
        _briWanderer.configure(1.0f, 3.0f, 2.0f, 0.5f, true, 0.0f, 1.0f);

        // Vignette wanderers (disabled — kept for easy re-enable)
        float midC = (_slot.cols - 1) * 0.5f;
        float midR = (_slot.rows - 1) * 0.5f;
        _vx.configure(midC, 1.2f, 1.5f, 0.35f, true, 0.0f, (float)(_slot.cols - 1));
        _vy.configure(midR, 1.2f, 1.5f, 0.35f, true, 0.0f, (float)(_slot.rows - 1));
        _vRadius.configure(5.0f, 0.25f, 1.5f, 0.3f, true, 3.5f, 7.5f);
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

        // Continuous piecewise-linear brightness map — three regions, shared endpoints:
        //   briPos [0.00, 0.40] → bri [0.00, 0.04]  (40% near-off, avg 0.02)
        //   briPos [0.40, 0.90] → bri [0.04, 0.35]  (50% dim-ambient, avg 0.20)
        //   briPos [0.90, 1.00] → bri [0.35, 0.75]  (10% bright flash, avg 0.55)
        float briPos = _briWanderer.pos;
        float globalBri;
        if (briPos < 0.40f) {
            float t = briPos / 0.40f;
            globalBri = t * 0.04f;
        } else if (briPos < 0.90f) {
            float t = (briPos - 0.40f) / 0.50f;
            globalBri = 0.04f + t * 0.31f;
        } else {
            float t = (briPos - 0.90f) / 0.10f;
            globalBri = 0.35f + t * 0.40f;
        }
        // Single brightness gate: shape rendered at full color, scaled here by wanderer × maxBri
        uint8_t scale = (uint8_t)(globalBri * _maxBri);
        for (uint16_t i = 0; i < _numLeds; i++) {
            _leds[i].nscale8(scale);
        }

        // Accumulate brightness stats (histogram on actual globalBri output)
        _briSum     += globalBri;
        _briSamples += 1;
        if (globalBri < _briMin) _briMin = globalBri;
        if (globalBri > _briMax) _briMax = globalBri;
        uint8_t bucket = (uint8_t)(globalBri * 5.0f);
        if (bucket > 4) bucket = 4;
        _briHist[bucket]++;

        if (nowMs - _lastDebugMs >= 1000) {
            _lastDebugMs = nowMs;
            auto& s = _slot.shapeState;
            Serial.printf("  [PAT] shape=%s briPos=%.2f bri=%.2f scale=%u/%u\n",
                          shape_name(_slot.culture.shape % SHAPE_COUNT),
                          briPos, globalBri, scale, (unsigned)_maxBri);
        }

        if (nowMs - _lastStatsMs >= 5000 && _briSamples > 0) {
            _lastStatsMs = nowMs;
            float mean = _briSum / (float)_briSamples;
            Serial.printf("  [BRI STATS] mean=%.2f min=%.2f max=%.2f n=%lu maxBri=%u\n",
                          mean, _briMin, _briMax, (unsigned long)_briSamples, (unsigned)_maxBri);
            Serial.printf("  [BRI HIST bri] 0-20%%:%lu 20-40%%:%lu 40-60%%:%lu 60-80%%:%lu 80-100%%:%lu\n",
                          (unsigned long)_briHist[0], (unsigned long)_briHist[1],
                          (unsigned long)_briHist[2], (unsigned long)_briHist[3],
                          (unsigned long)_briHist[4]);
            _briSum = 0.0f; _briSamples = 0; _briMin = 1.0f; _briMax = 0.0f;
            memset(_briHist, 0, sizeof(_briHist));
        }
    }

private:
    CRGB*       _leds        = nullptr;
    uint16_t    _numLeds     = 0;
    uint32_t    _lastMs      = 0;
    uint32_t    _lastDebugMs = 0;
    PatternType _patternType = PATTERN_SHAPE;
    PatternSlot _slot;
    uint8_t     _maxBri      = 255;
    Wanderer    _briWanderer;
    Wanderer    _vx, _vy, _vRadius;  // vignette spotlight center + radius (disabled)

    // Brightness distribution stats
    float    _briSum     = 0.0f;
    uint32_t _briSamples = 0;
    float    _briMin     = 1.0f;
    float    _briMax     = 0.0f;
    uint32_t _briHist[5] = {};
    uint32_t _lastStatsMs = 0;
};

} // namespace pulleys
