#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <pulleys_protocol.h>
#include <pulleys_identity.h>

// ── Common identify-over-serial interface ────────────────────────────────────
//
// Every role answers the same one-character request with the same one-line
// reply, so a host tool can ask a board what it is instead of inferring it from
// the port name or from role-specific log chatter.
//
//   request:  "?\n"
//   reply:    "PULLEYS-ID v=1 type=4 role=SENSOR env=sensor class=s3_4mb
//              id=A855 label=0 name=N-A855 mac=3C:0F:02:E4:52:2C"   (one line)
//
// Two independent answers, because flashing needs both:
//   what it is   — `type` (PULLEYS_TYPE_* enum) and `role` (its name); `env`
//                  names the PlatformIO environment, separating the pairs the
//                  enum cannot: arbiter vs arbiter_mesh, station vs
//                  station_wroom (same role, different firmware).
//   what it runs on — `class`, the hardware class, read at runtime from the
//                  chip and its flash/PSRAM. This is what decides whether an
//                  image will fit, and it stays truthful even when the flashed
//                  role is wrong.
//   which one    — `id`, the stable MAC-derived device ID (also `mac`, and
//                  `label` for its registry number). Lets a host address one
//                  specific board rather than every board of a class.
//
// The reply is also emitted once at boot, so a listener that catches startup
// does not have to ask.
//
// Format rules, for anything parsing this:
//   - The line always starts with PULLEYS_ID_PREFIX and is a single \n line.
//   - Fields are space-separated key=value, values never contain spaces.
//   - Parse by key, not position. New keys may be appended; bump v= only if an
//     existing key changes meaning.

#ifndef PULLEYS_ENV
#define PULLEYS_ENV "unknown"      // platformio.ini injects the real env name
#endif

#define PULLEYS_ID_PREFIX  "PULLEYS-ID"
#define PULLEYS_ID_VERSION 1
#define PULLEYS_ID_REQUEST '?'

namespace pulleys {

inline const char* whoami_role_name(uint8_t type) {
    switch (type) {
        case PULLEYS_TYPE_STATION:  return "STATION";
        case PULLEYS_TYPE_TRAVELER: return "TRAVELER";
        case PULLEYS_TYPE_ARBITER:  return "ARBITER";
        case PULLEYS_TYPE_SENSOR:   return "SENSOR";
        case PULLEYS_TYPE_SCREEN:   return "SCREEN";
        default:                    return "UNKNOWN";
    }
}

// Hardware class, determined at runtime rather than compiled in, so it still
// reports the truth about a board carrying the wrong firmware. Names match the
// classes flash_all.sh uses to decide what an image may be written to.
inline const char* whoami_hw_class() {
#if CONFIG_IDF_TARGET_ESP32S3
    // The two S3 boards differ by memory fit: 16MB/8MB arbiter vs 4MB/2MB
    // traveler-class. PSRAM is the sharper split, flash size the backstop.
    if (ESP.getPsramSize() > (4u << 20) || ESP.getFlashChipSize() > (8u << 20)) return "s3_16mb";
    return "s3_4mb";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "c3";
#elif CONFIG_IDF_TARGET_ESP32
    return "esp32";
#else
    return "unknown";
#endif
}

// Emit the identity line. Safe to call before identity_init(): the MAC is read
// directly here, so it is always right, and id/name simply read back as unset.
//
// The reported MAC is the base (WiFi STA) address — what esptool prints and
// what the mesh logs use — so a host can match this board against either.
// Note identity_id() is hashed from ESP_MAC_BT, which is the base MAC + 2 on
// these parts; that hash input is deliberately left alone, since changing it
// would renumber every device ID in the registries.
inline void whoami_reply() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const char* name = identity_name();
    Serial.printf("%s v=%d type=%u role=%s env=%s class=%s id=%04X label=%u name=%s "
                  "mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  PULLEYS_ID_PREFIX, PULLEYS_ID_VERSION,
                  (unsigned)PULLEYS_DEVICE_TYPE,
                  whoami_role_name(PULLEYS_DEVICE_TYPE),
                  PULLEYS_ENV,
                  whoami_hw_class(),
                  identity_id(),
                  (unsigned)identity_label(),
                  (name && name[0]) ? name : "?",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Hook for roles that already parse line-oriented serial commands: pass the
// completed line, returns true if it was an identify request and was answered.
inline bool whoami_handle(const char* line) {
    if (line && line[0] == PULLEYS_ID_REQUEST) {
        whoami_reply();
        return true;
    }
    return false;
}

// Standalone pump for roles with no other serial commands. Call from loop().
// Answers '?' and ignores everything else, so it never swallows input a role
// might later want.
inline void whoami_poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == PULLEYS_ID_REQUEST) whoami_reply();
    }
}

} // namespace pulleys
