#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <pulleys_protocol.h>

// ── Device identity derived from ESP32 MAC address ────────────────────────────
// Provides a stable 16-bit ID and human-readable name (e.g. "T-A3F2", "S-7B01").

namespace pulleys {

static uint16_t _deviceId = 0;
static char     _deviceName[8] = {0};
static uint8_t  _mac[6] = {0};

// Call once in setup() after WiFi/BLE init (MAC must be available).
inline void identity_init(uint8_t deviceType) {
    esp_read_mac(_mac, ESP_MAC_BT);

    // Hash last 4 bytes of MAC into a 16-bit ID
    uint32_t hash = _mac[2] ^ (_mac[3] << 3) ^ (_mac[4] << 7) ^ (_mac[5] << 11);
    hash = (hash * 2654435761U) >> 16;   // Knuth multiplicative hash, take upper 16 bits
    _deviceId = (uint16_t)(hash & 0xFFFF);

    char prefix = (deviceType == PULLEYS_TYPE_TRAVELER) ? 'T' : 'S';
    snprintf(_deviceName, sizeof(_deviceName), "%c-%04X", prefix, _deviceId);
}

inline uint16_t identity_id()   { return _deviceId; }
inline const char* identity_name() { return _deviceName; }

// Print boot banner with identity info to Serial
inline void identity_print_banner(uint8_t deviceType) {
    const char* typeName = (deviceType == PULLEYS_TYPE_TRAVELER) ? "Traveler" : "Station";
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS %s\n", typeName);
    Serial.printf("  ID:  %s (0x%04X)\n", _deviceName, _deviceId);
    Serial.printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

} // namespace pulleys
