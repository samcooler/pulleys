#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <pulleys_protocol.h>

// ── Proximity detection via smoothed BLE RSSI ─────────────────────────────────
// Tracks nearby devices by ID, classifies into zones, fires callbacks on change.

namespace pulleys {

enum ProximityZone : uint8_t {
    ZONE_GONE = 0,   // not seen recently
    ZONE_FAR,        // weak signal
    ZONE_NEAR,       // moderate signal
    ZONE_CLOSE       // strong signal — culture exchange range
};

inline const char* zone_name(ProximityZone z) {
    switch (z) {
        case ZONE_CLOSE: return "CLOSE";
        case ZONE_NEAR:  return "NEAR";
        case ZONE_FAR:   return "FAR";
        default:         return "GONE";
    }
}

struct TrackedDevice {
    uint16_t       deviceId    = 0;
    uint8_t        deviceType  = 0;
    PulleysCulture culture     = {};
    float          rssiSmooth  = -100.0f;
    ProximityZone  zone        = ZONE_GONE;
    uint32_t       lastSeenMs  = 0;
    bool           active      = false;
};

// Callback signature: called when a device changes zone
typedef void (*ZoneChangeCallback)(const TrackedDevice& device, ProximityZone oldZone, ProximityZone newZone);

class ProximityTracker {
public:
    static constexpr uint8_t  MAX_TRACKED    = 32;
    static constexpr float    RSSI_ALPHA     = 0.3f;    // smoothing factor (higher = more responsive)
    static constexpr int      RSSI_CLOSE     = -58;     // dBm threshold for CLOSE
    static constexpr int      RSSI_NEAR      = -73;     // dBm threshold for NEAR
    static constexpr int      RSSI_FAR       = -80;     // dBm threshold for FAR
    static constexpr int      HYSTERESIS     = 5;       // dBm hysteresis to prevent zone flickering
    static constexpr uint32_t TIMEOUT_MS     = 10000;   // remove device after 10s silence

    void setZoneChangeCallback(ZoneChangeCallback cb) { _callback = cb; }

    // Feed a received advertisement. Call from BLE scan callback.
    void update(const PulleysPacket& pkt, int rssi) {
        TrackedDevice* dev = _findOrCreate(pkt.deviceId);
        if (!dev) return;  // table full

        dev->deviceType = pkt.deviceType;
        dev->culture    = pkt.culture;
        dev->lastSeenMs = millis();
        dev->active     = true;

        // Exponential moving average of RSSI
        if (dev->rssiSmooth < -99.0f) {
            dev->rssiSmooth = (float)rssi;  // first reading
        } else {
            dev->rssiSmooth = dev->rssiSmooth * (1.0f - RSSI_ALPHA) + rssi * RSSI_ALPHA;
        }

        // Classify zone with hysteresis
        ProximityZone newZone = _classify(dev->rssiSmooth, dev->zone);
        if (newZone != dev->zone) {
            ProximityZone oldZone = dev->zone;
            dev->zone = newZone;
            Serial.printf("  [PROX] %c-%04X: %s → %s (RSSI %.0f dBm)\n",
                          dev->deviceType == PULLEYS_TYPE_TRAVELER ? 'T' : 'S',
                          dev->deviceId,
                          zone_name(oldZone), zone_name(newZone),
                          dev->rssiSmooth);
            if (_callback) _callback(*dev, oldZone, newZone);
        }
    }

    // Call periodically (~1Hz) to expire stale devices
    void pruneStale() {
        uint32_t now = millis();
        for (uint8_t i = 0; i < MAX_TRACKED; i++) {
            if (_devices[i].active && (now - _devices[i].lastSeenMs > TIMEOUT_MS)) {
                if (_devices[i].zone != ZONE_GONE) {
                    ProximityZone old = _devices[i].zone;
                    _devices[i].zone = ZONE_GONE;
                    Serial.printf("  [PROX] %c-%04X: %s → GONE (timeout)\n",
                                  _devices[i].deviceType == PULLEYS_TYPE_TRAVELER ? 'T' : 'S',
                                  _devices[i].deviceId, zone_name(old));
                    if (_callback) _callback(_devices[i], old, ZONE_GONE);
                }
                _devices[i].active = false;
            }
        }
    }

    // Get tracked device by ID (or nullptr)
    const TrackedDevice* getDevice(uint16_t id) const {
        for (uint8_t i = 0; i < MAX_TRACKED; i++) {
            if (_devices[i].active && _devices[i].deviceId == id) return &_devices[i];
        }
        return nullptr;
    }

    // Count devices in a given zone
    uint8_t countInZone(ProximityZone zone) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < MAX_TRACKED; i++) {
            if (_devices[i].active && _devices[i].zone == zone) n++;
        }
        return n;
    }

    // Iterate all active devices. Callback receives const ref.
    template<typename Func>
    void forEachActive(Func fn) const {
        for (uint8_t i = 0; i < MAX_TRACKED; i++) {
            if (_devices[i].active) fn(_devices[i]);
        }
    }

private:
    TrackedDevice     _devices[MAX_TRACKED] = {};
    ZoneChangeCallback _callback = nullptr;

    TrackedDevice* _findOrCreate(uint16_t id) {
        TrackedDevice* empty = nullptr;
        for (uint8_t i = 0; i < MAX_TRACKED; i++) {
            if (_devices[i].active && _devices[i].deviceId == id) return &_devices[i];
            if (!_devices[i].active && !empty) empty = &_devices[i];
        }
        if (empty) {
            *empty = TrackedDevice{};
            empty->deviceId = id;
        }
        return empty;
    }

    ProximityZone _classify(float rssi, ProximityZone current) {
        // Apply hysteresis: require crossing threshold + hysteresis to move up,
        // or threshold - hysteresis to move down
        int h = HYSTERESIS;
        if (rssi >= RSSI_CLOSE + (current >= ZONE_CLOSE ? -h : h))
            return ZONE_CLOSE;
        if (rssi >= RSSI_NEAR + (current >= ZONE_NEAR ? -h : h))
            return ZONE_NEAR;
        if (rssi >= RSSI_FAR + (current >= ZONE_FAR ? -h : h))
            return ZONE_FAR;
        return ZONE_GONE;
    }
};

} // namespace pulleys
