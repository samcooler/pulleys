#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Shared BLE beacon protocol ────────────────────────────────────────────────
// Packet layout (manufacturer data, 6 bytes):
//   [0..1]  Company ID  (little-endian, PULLEYS_COMPANY_ID)
//   [2..5]  Counter     (uint32_t, little-endian)

#define PULLEYS_DEVICE_NAME  "pulleys"
#define PULLEYS_COMPANY_ID   0xFFFF   // 0xFFFF = development / unregistered
#define PULLEYS_MFR_LEN      6

typedef struct {
    uint32_t counter;
} PulleysPacket;

inline void pulleys_serialize(const PulleysPacket* pkt, uint8_t out[PULLEYS_MFR_LEN]) {
    out[0] = PULLEYS_COMPANY_ID & 0xFF;
    out[1] = (PULLEYS_COMPANY_ID >> 8) & 0xFF;
    out[2] = (pkt->counter >>  0) & 0xFF;
    out[3] = (pkt->counter >>  8) & 0xFF;
    out[4] = (pkt->counter >> 16) & 0xFF;
    out[5] = (pkt->counter >> 24) & 0xFF;
}

inline bool pulleys_parse(const uint8_t* data, size_t len, PulleysPacket* pkt) {
    if (len < PULLEYS_MFR_LEN) return false;
    if (data[0] != (PULLEYS_COMPANY_ID & 0xFF))        return false;
    if (data[1] != ((PULLEYS_COMPANY_ID >> 8) & 0xFF)) return false;
    pkt->counter = (uint32_t)data[2]
                 | ((uint32_t)data[3] <<  8)
                 | ((uint32_t)data[4] << 16)
                 | ((uint32_t)data[5] << 24);
    return true;
}
