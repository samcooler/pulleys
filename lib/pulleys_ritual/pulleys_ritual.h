#pragma once

#include <stdint.h>
#include <Arduino.h>

// ── Ritual detection via IMU (QMI8658) ────────────────────────────────────────
// Stub framework for detecting gestures that modify culture interactions.
// Travelers carry 6-axis IMU; ritual gestures could amplify or alter
// how culture is exchanged during a close encounter.

namespace pulleys {

enum RitualGesture : uint8_t {
    GESTURE_NONE = 0,
    GESTURE_SHAKE,       // vigorous shaking
    GESTURE_SPIN,        // rotation
    GESTURE_HOLD_STILL,  // deliberate stillness
    // TODO: add more gestures as we discover what feels good
};

inline const char* gesture_name(RitualGesture g) {
    switch (g) {
        case GESTURE_SHAKE:      return "SHAKE";
        case GESTURE_SPIN:       return "SPIN";
        case GESTURE_HOLD_STILL: return "HOLD_STILL";
        default:                 return "NONE";
    }
}

class RitualDetector {
public:
    // Call once in setup() after IMU is initialized
    void init() {
        // TODO: configure thresholds for gesture detection
        Serial.println("  [RITUAL] Detector initialized (stub)");
    }

    // Feed accelerometer + gyroscope readings. Call at ~10–50 Hz.
    void update(float ax, float ay, float az, float gx, float gy, float gz) {
        (void)ax; (void)ay; (void)az;
        (void)gx; (void)gy; (void)gz;
        // TODO: implement gesture detection
        // - SHAKE: high accel magnitude variance over short window
        // - SPIN: sustained high gyroscope reading on one axis
        // - HOLD_STILL: low accel variance + low gyro for several seconds
        _currentGesture = GESTURE_NONE;
    }

    RitualGesture currentGesture() const { return _currentGesture; }

    // How much should the current gesture amplify culture exchange?
    // Returns a multiplier: 1.0 = normal, >1.0 = amplified
    float exchangeMultiplier() const {
        // TODO: different gestures could amplify/dampen/transform exchange
        return 1.0f;
    }

private:
    RitualGesture _currentGesture = GESTURE_NONE;
};

} // namespace pulleys
