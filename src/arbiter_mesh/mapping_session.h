#pragma once
#include <stdint.h>

// Hardware-independent capture/ownership rules, shared by touch and remote UI.
// owner == 0, bridge == 0 is the touchscreen and does not expire.
struct MappingSession {
    enum Phase : uint8_t { WAIT, LISTEN, PICK };
    static constexpr uint32_t LISTEN_MS = 5000;
    static constexpr uint32_t LEASE_MS = 30000;
    static constexpr uint8_t MAX_CAPTURE = 10;
    bool active = false;
    uint64_t owner = 0;
    uint16_t bridge = 0;
    Phase phase = WAIT;
    uint16_t ids[MAX_CAPTURE] = {};
    uint8_t count = 0;
    int8_t assigned = -1;
    uint32_t listenAt = 0;
    uint32_t renewedAt = 0;

    bool owns(uint64_t token, uint16_t source) const {
        return active && owner == token && bridge == source;
    }
    void reset() { phase = WAIT; count = 0; assigned = -1; }
    void tick(uint32_t now) {
        if (!active) return;
        if (owner && (uint32_t)(now - renewedAt) >= LEASE_MS) {
            active = false; reset(); return;
        }
        if (phase == LISTEN && (uint32_t)(now - listenAt) >= LISTEN_MS) phase = PICK;
    }
    bool open(uint64_t token, uint16_t source, uint32_t now) {
        tick(now);
        if (active && !owns(token, source)) return false;
        if (!active) { reset(); owner = token; bridge = source; active = true; }
        renewedAt = now;
        return true;
    }
    bool renew(uint64_t token, uint16_t source, uint32_t now) {
        tick(now);
        if (!owns(token, source)) return false;
        renewedAt = now; return true;
    }
    bool close(uint64_t token, uint16_t source) {
        if (!owns(token, source)) return false;
        active = false; reset(); return true;
    }
    bool contains(uint16_t id) const {
        for (uint8_t i = 0; i < count; ++i) if (ids[i] == id) return true;
        return false;
    }
    void pull(uint16_t id, uint32_t now) {
        tick(now);
        if (!active) return;
        bool known = contains(id);
        if (phase == WAIT || (phase == PICK && assigned >= 0 && !known)) {
            reset(); ids[count++] = id; phase = LISTEN; listenAt = now;
        } else if (phase == LISTEN && !known && count < MAX_CAPTURE) ids[count++] = id;
    }
};
