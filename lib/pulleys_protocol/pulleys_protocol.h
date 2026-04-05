#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ── Shared BLE beacon protocol ────────────────────────────────────────────────
//
// Manufacturer data layout (16 bytes total):
//   [0..1]   Company ID    (little-endian, PULLEYS_COMPANY_ID)
//   [2]      Device type   (0x01 = Station, 0x02 = Traveler)
//   [3..4]   Device ID     (uint16_t, little-endian, derived from MAC)
//   [5..7]   Color A       (RGB, 3 bytes)
//   [8..10]  Color B       (RGB, 3 bytes)
//   [11]     Oscillation   (frequency byte: 1–255, maps to ~0.1–5 Hz)
//   [12..15] Counter       (uint32_t, little-endian)
//
// No device name is broadcast — filtering uses company ID + packet structure.

#define PULLEYS_COMPANY_ID   0xFFFF   // 0xFFFF = development / unregistered
#define PULLEYS_MFR_LEN      16

#define PULLEYS_TYPE_STATION   0x01
#define PULLEYS_TYPE_TRAVELER  0x02

typedef struct {
    uint8_t r, g, b;
} PulleysColor;

typedef struct {
    PulleysColor colorA;
    PulleysColor colorB;
    uint8_t      oscillation;   // 1–255, maps to ~0.1–5.0 Hz
} PulleysCulture;

typedef struct {
    uint8_t        deviceType;  // PULLEYS_TYPE_STATION or PULLEYS_TYPE_TRAVELER
    uint16_t       deviceId;    // stable ID derived from MAC
    PulleysCulture culture;
    uint32_t       counter;
} PulleysPacket;

// ── Serialize packet into 16-byte manufacturer data buffer ────────────────────
inline void pulleys_serialize(const PulleysPacket* pkt, uint8_t out[PULLEYS_MFR_LEN]) {
    out[0]  = PULLEYS_COMPANY_ID & 0xFF;
    out[1]  = (PULLEYS_COMPANY_ID >> 8) & 0xFF;
    out[2]  = pkt->deviceType;
    out[3]  = pkt->deviceId & 0xFF;
    out[4]  = (pkt->deviceId >> 8) & 0xFF;
    out[5]  = pkt->culture.colorA.r;
    out[6]  = pkt->culture.colorA.g;
    out[7]  = pkt->culture.colorA.b;
    out[8]  = pkt->culture.colorB.r;
    out[9]  = pkt->culture.colorB.g;
    out[10] = pkt->culture.colorB.b;
    out[11] = pkt->culture.oscillation;
    out[12] = (pkt->counter >>  0) & 0xFF;
    out[13] = (pkt->counter >>  8) & 0xFF;
    out[14] = (pkt->counter >> 16) & 0xFF;
    out[15] = (pkt->counter >> 24) & 0xFF;
}

// ── Parse 16-byte manufacturer data into packet struct ────────────────────────
inline bool pulleys_parse(const uint8_t* data, size_t len, PulleysPacket* pkt) {
    if (len < PULLEYS_MFR_LEN) return false;
    if (data[0] != (PULLEYS_COMPANY_ID & 0xFF))        return false;
    if (data[1] != ((PULLEYS_COMPANY_ID >> 8) & 0xFF)) return false;

    pkt->deviceType          = data[2];
    pkt->deviceId            = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    pkt->culture.colorA.r    = data[5];
    pkt->culture.colorA.g    = data[6];
    pkt->culture.colorA.b    = data[7];
    pkt->culture.colorB.r    = data[8];
    pkt->culture.colorB.g    = data[9];
    pkt->culture.colorB.b    = data[10];
    pkt->culture.oscillation = data[11];
    pkt->counter             = (uint32_t)data[12]
                             | ((uint32_t)data[13] <<  8)
                             | ((uint32_t)data[14] << 16)
                             | ((uint32_t)data[15] << 24);
    return true;
}
