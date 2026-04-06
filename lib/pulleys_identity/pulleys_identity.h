#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <pulleys_protocol.h>

// ── Device identity derived from ESP32 MAC address ────────────────────────────
// Provides a stable 16-bit ID and human-readable name (e.g. "T-A3F2", "S-7B01").

namespace pulleys {

static uint16_t _deviceId = 0;
static uint8_t  _deviceLabel = 0;
static char     _deviceName[8] = {0};
static uint8_t  _mac[6] = {0};

// ── Board registry: map known device IDs to physical label numbers ────────────
// Travelers and stations are numbered independently, both starting at 1.
struct BoardEntry { uint16_t id; uint8_t label; };
static const BoardEntry _travelerRegistry[] = {
    { 0x6910,  1 },  // T-6910
    { 0xA08A,  2 },  // T-A08A
    { 0x194A,  3 },  // T-194A
    // Add new travelers here: { 0xXXXX, NN },
};
static const BoardEntry _stationRegistry[] = {
    { 0xF563,  1 },  // S-F563 — XIAO C3
    { 0xD25E,  2 },  // S-D25E — XIAO C3
    { 0xD3C5,  3 },  // S-D3C5 — XIAO C3
    // Add new stations here: { 0xXXXX, NN },
};
static constexpr uint8_t _travelerRegistrySize = sizeof(_travelerRegistry) / sizeof(_travelerRegistry[0]);
static constexpr uint8_t _stationRegistrySize  = sizeof(_stationRegistry)  / sizeof(_stationRegistry[0]);

// Call once in setup() after WiFi/BLE init (MAC must be available).
inline void identity_init(uint8_t deviceType) {
    esp_read_mac(_mac, ESP_MAC_BT);

    // Hash last 4 bytes of MAC into a 16-bit ID
    uint32_t hash = _mac[2] ^ (_mac[3] << 3) ^ (_mac[4] << 7) ^ (_mac[5] << 11);
    hash = (hash * 2654435761U) >> 16;   // Knuth multiplicative hash, take upper 16 bits
    _deviceId = (uint16_t)(hash & 0xFFFF);

    // Look up label from the appropriate registry
    _deviceLabel = 0;  // 0 = unknown board
    const BoardEntry* reg = (deviceType == PULLEYS_TYPE_TRAVELER) ? _travelerRegistry : _stationRegistry;
    uint8_t regSize = (deviceType == PULLEYS_TYPE_TRAVELER) ? _travelerRegistrySize : _stationRegistrySize;
    for (uint8_t i = 0; i < regSize; i++) {
        if (reg[i].id == _deviceId) {
            _deviceLabel = reg[i].label;
            break;
        }
    }

    char prefix = (deviceType == PULLEYS_TYPE_TRAVELER) ? 'T' : 'S';
    snprintf(_deviceName, sizeof(_deviceName), "%c-%04X", prefix, _deviceId);
}

inline uint16_t identity_id()    { return _deviceId; }
inline uint8_t  identity_label() { return _deviceLabel; }
inline const char* identity_name() { return _deviceName; }

// Print boot banner with identity info to Serial
inline void identity_print_banner(uint8_t deviceType) {
    const char* typeName = (deviceType == PULLEYS_TYPE_TRAVELER) ? "Traveler" : "Station";
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS %s\n", typeName);
    Serial.printf("  ID:  %s (0x%04X)  Label: #%02d\n", _deviceName, _deviceId, _deviceLabel);
    Serial.printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

} // namespace pulleys
