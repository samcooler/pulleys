#pragma once
#include <stdint.h>
#include <pulleys_protocol.h>

#define ARBITER_MAX_DEVICES  24
#define RSSI_HISTORY_LEN     120   // 1 sample/sec → 2-min window
#define DEVICE_TIMEOUT_MS    30000

struct ArbiterDevice {
    bool           active;
    uint8_t        type;           // PULLEYS_TYPE_TRAVELER / _STATION
    uint16_t       id;
    uint8_t        mac[6];
    PulleysCulture culture;
    int8_t         current_rssi;
    int8_t         rssi_buf[RSSI_HISTORY_LEN];
    uint8_t        rssi_head;      // next write index (ring buffer)
    uint8_t        rssi_count;     // valid samples, capped at RSSI_HISTORY_LEN
    uint32_t       last_seen_ms;
    uint32_t       first_seen_ms;
};

// Call once from setup() — inits NimBLE and starts passive scan
void scanner_init();

// Call every ~1000ms from loop() — ages out stale devices, ticks RSSI into ring buffer
void scanner_tick();

// Read-only view of the device table
const ArbiterDevice* scanner_devices();
uint8_t scanner_count();
