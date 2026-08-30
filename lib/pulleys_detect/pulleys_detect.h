#pragma once

#include <stdint.h>
#include <math.h>
#include <Arduino.h>

// ── pulleys_detect — motion-bout detection for the Sensor role ────────────────
//
// One state machine, two modes:
//
//   ROTATION — integrate gyro rate about the dominant axis of the bout.
//              Fires when |angle| crosses a threshold (180° or 260°).
//
//   LINEAR   — strip gravity with a slow EMA, learn the "primary direction" as
//              the dominant eigenvector of the residual covariance (power
//              iteration, one step per sample), then fire when the impulse
//              along that axis crosses a threshold.
//
//   IDLE ──(motion starts)──▶ ACTIVE ──(measure over threshold)──▶ fire ──▶ REFRACTORY
//        ◀──────────────────── (still for holdoff) ────────────────┘
//
// Bouts are short, so the integration terms never drift far and the
// accumulator zeroes on every bout end.

namespace pulleys {

struct DetectConfig {
    uint8_t mode = 0;                  // SENSOR_MODE_ROTATION / _LINEAR

    // Rotation
    float rotThresholdDeg = 180.0f;    // 180 or 260
    float rotDeadbandDps  = 3.0f;      // ignore rate below this (bias rejection)
    float rotStartDps     = 25.0f;     // rate that opens a bout

    // Linear
    float linStartG       = 0.12f;     // residual accel magnitude that opens a bout
    float linImpulseGs    = 0.22f;     // along-axis impulse (g·s) needed to fire
    float linMinDurMs     = 180.0f;    // and it must last at least this long

    // Shared
    float    gravityTau   = 0.6f;      // s — EMA time constant for the gravity estimate
    uint32_t holdoffMs    = 400;       // stillness that closes an unfired bout
    uint32_t refractoryMs = 1500;      // cooldown after firing
};

enum DetectState : uint8_t { DET_IDLE = 0, DET_ACTIVE, DET_REFRACTORY };

class Detector {
public:
    DetectConfig cfg;

    void init(const DetectConfig& c) {
        cfg = c;
        _state = DET_IDLE;
        _gx = _gy = _gz = 0.0f;
        _gravValid = false;
        // Seed the learned axis with something non-degenerate; it converges fast.
        _ax = 1.0f; _ay = 0.0f; _az = 0.0f;
        reset();
    }

    // Feed a sample. accel in g, gyro in deg/s, dt in seconds.
    // Returns true exactly once per detection; magnitude is quantized 1..255.
    bool update(float axg, float ayg, float azg,
                float wx, float wy, float wz,
                float dt, uint8_t& magnitudeOut) {
        uint32_t now = millis();

        if (_state == DET_REFRACTORY) {
            if (now - _stateMs >= cfg.refractoryMs) { _state = DET_IDLE; reset(); }
            return false;
        }

        // Gravity estimate → linear residual
        float alpha = dt / (cfg.gravityTau + dt);
        if (!_gravValid) { _gx = axg; _gy = ayg; _gz = azg; _gravValid = true; }
        else {
            _gx += (axg - _gx) * alpha;
            _gy += (ayg - _gy) * alpha;
            _gz += (azg - _gz) * alpha;
        }
        float rx = axg - _gx, ry = ayg - _gy, rz = azg - _gz;
        float rmag = sqrtf(rx*rx + ry*ry + rz*rz);
        float wmag = sqrtf(wx*wx + wy*wy + wz*wz);

        _lastResidual = rmag;
        _lastRate     = wmag;

        bool moving = (cfg.mode == 0) ? (wmag >= cfg.rotStartDps)
                                      : (rmag >= cfg.linStartG);

        if (_state == DET_IDLE) {
            if (!moving) return false;
            _state   = DET_ACTIVE;
            _stateMs = now;
            _quietMs = now;
            reset();
        }

        // ── ACTIVE ──
        if (moving) _quietMs = now;

        if (cfg.mode == 0) {
            // Integrate each axis separately; the dominant one is the rotation axis.
            if (fabsf(wx) > cfg.rotDeadbandDps) _rotX += wx * dt;
            if (fabsf(wy) > cfg.rotDeadbandDps) _rotY += wy * dt;
            if (fabsf(wz) > cfg.rotDeadbandDps) _rotZ += wz * dt;

            float best = fabsf(_rotX);
            if (fabsf(_rotY) > best) best = fabsf(_rotY);
            if (fabsf(_rotZ) > best) best = fabsf(_rotZ);
            _measure = best;

            if (best >= cfg.rotThresholdDeg) {
                float q = best * 0.5f;                       // degrees / 2
                magnitudeOut = (uint8_t)(q > 255.0f ? 255.0f : (q < 1.0f ? 1.0f : q));
                fire(now);
                return true;
            }
        } else {
            // Power iteration on the residual covariance: axis += r * (r·axis).
            float proj = rx*_ax + ry*_ay + rz*_az;
            float lr   = 0.02f * rmag;                        // learn faster on strong motion
            _ax += lr * rx * proj;  _ay += lr * ry * proj;  _az += lr * rz * proj;
            float n = sqrtf(_ax*_ax + _ay*_ay + _az*_az);
            if (n > 1e-6f) { _ax /= n; _ay /= n; _az /= n; }

            _impulse += fabsf(proj) * dt;
            _measure  = _impulse;

            if (_impulse >= cfg.linImpulseGs &&
                (now - _stateMs) >= (uint32_t)cfg.linMinDurMs) {
                float q = _impulse * 200.0f;                  // ~0.005 g·s per step
                magnitudeOut = (uint8_t)(q > 255.0f ? 255.0f : (q < 1.0f ? 1.0f : q));
                fire(now);
                return true;
            }
        }

        // Bout abandoned — went quiet without reaching the threshold
        if (now - _quietMs >= cfg.holdoffMs) { _state = DET_IDLE; reset(); }
        return false;
    }

    DetectState state() const     { return _state; }
    float       measure() const   { return _measure; }      // deg, or g·s
    float       lastRate() const  { return _lastRate; }
    float       lastResid() const { return _lastResidual; }
    void axis(float& x, float& y, float& z) const { x = _ax; y = _ay; z = _az; }

private:
    DetectState _state = DET_IDLE;
    uint32_t _stateMs = 0, _quietMs = 0;

    float _gx = 0, _gy = 0, _gz = 0;      // gravity estimate
    bool  _gravValid = false;
    float _ax = 1, _ay = 0, _az = 0;      // learned primary direction
    float _rotX = 0, _rotY = 0, _rotZ = 0;
    float _impulse = 0;
    float _measure = 0, _lastRate = 0, _lastResidual = 0;

    void reset() { _rotX = _rotY = _rotZ = 0.0f; _impulse = 0.0f; _measure = 0.0f; }

    void fire(uint32_t now) {
        _state   = DET_REFRACTORY;
        _stateMs = now;
        reset();
    }
};

}  // namespace pulleys
