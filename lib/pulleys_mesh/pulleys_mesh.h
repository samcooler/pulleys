#pragma once

#include <stdint.h>
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── pulleys_mesh — connectionless ESP-NOW broadcast with a flood relay ────────
//
// Every node broadcasts on a fixed Wi-Fi channel. Sensors emit small EVENT
// packets; all nodes dedupe by (originId, seq), act once, and rebroadcast unseen
// events with a decrementing TTL. No AP, no pairing, no routing state.
//
// v1: EVENT packets only. DIGEST is reserved for the game/puzzle layer later.
//
// Usage:
//   pulleys::mesh_init(pulleys::MESH_ORIGIN_SENSOR, id, MESH_WIFI_CHANNEL);
//   pulleys::mesh_on_event(myCallback);              // screens / relays
//   pulleys::mesh_send_event(channel, mode, mag, 0); // sensors, on detection
//   pulleys::mesh_poll();                            // every loop()

#ifndef MESH_WIFI_CHANNEL
#define MESH_WIFI_CHANNEL 1
#endif

namespace pulleys {

enum : uint8_t {
    MESH_ORIGIN_SENSOR = 0x04,
    MESH_ORIGIN_SCREEN = 0x05,
};

enum : uint8_t {
    MESH_MSG_EVENT  = 0x10,
    MESH_MSG_DIGEST = 0x20,   // reserved
};

enum : uint8_t {
    SENSOR_MODE_ROTATION = 0,
    SENSOR_MODE_LINEAR   = 1,
};

static constexpr uint8_t  MESH_TTL_START = 3;
static constexpr uint8_t  MESH_BURST     = 6;    // copies per local detection
static constexpr uint16_t MESH_BURST_GAP = 160;  // ms between burst copies

struct __attribute__((packed)) MeshEvent {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_EVENT
    uint8_t  originType;  // MESH_ORIGIN_*
    uint16_t originId;    // stable per-node id
    uint16_t seq;         // per-origin sequence (dedupe key with originId)
    uint8_t  ttl;         // remaining flood hops
    uint8_t  channel;     // 0..15
    uint8_t  mode;        // SENSOR_MODE_*
    uint8_t  magnitude;   // quantized strength (degrees/2 or impulse level)
    uint8_t  flags;       // bit0 = battery low
    uint8_t  reserved;
};  // 14 bytes

typedef void (*MeshEventCb)(const MeshEvent& ev, bool relayed);

// ── Internal state ──────────────────────────────────────────────────────────
namespace _mesh {

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t     originType = 0;
static uint16_t    originId   = 0;
static uint16_t    seqCounter = 0;
static MeshEventCb eventCb    = nullptr;

// Deduplication ring — linear scan, fine at this node count / event rate.
static uint32_t seen[512];
static uint16_t seenHead = 0;

inline bool wasSeen(uint32_t key) {
    for (uint16_t i = 0; i < 512; i++) if (seen[i] == key) return true;
    return false;
}
inline void markSeen(uint32_t key) {
    seen[seenHead] = key;
    seenHead = (seenHead + 1) & 511;
}
inline uint32_t keyOf(uint16_t id, uint16_t sq) {
    return ((uint32_t)id << 16) | sq;
}

// Outbound queue — handles both local bursts and relay resends.
struct OutSlot {
    MeshEvent ev;
    uint32_t  dueMs;
    uint16_t  gapMs;
    uint8_t   left;
    bool      used;
};
static OutSlot out[16];

inline void enqueue(const MeshEvent& ev, uint8_t count, uint16_t gap, uint16_t firstDelay) {
    for (uint8_t i = 0; i < 16; i++) {
        if (out[i].used) continue;
        out[i].ev    = ev;
        out[i].left  = count;
        out[i].gapMs = gap;
        out[i].dueMs = millis() + firstDelay;
        out[i].used  = true;
        return;
    }
    // queue full — drop silently (bursts make this non-fatal)
}

// RX ring — WiFi task is the single producer, mesh_poll() the single consumer.
// Producer is the WiFi task (a real FreeRTOS task, not an ISR), consumer is
// mesh_poll() on the main loop — single producer / single consumer, so volatile
// indices around a plain slot array are sufficient.
static MeshEvent          rx[8];
static volatile uint8_t   rxHead = 0;
static volatile uint8_t   rxTail = 0;

// Diagnostics — every packet the radio hands us, before and after filtering.
static volatile uint32_t statRaw      = 0;   // frames delivered by esp-now
static volatile uint32_t statBadMagic = 0;   // not ours
static volatile uint32_t statRingFull = 0;   // dropped, consumer too slow
static uint32_t          statSent     = 0;   // esp_now_send calls
static uint32_t          statSendErr  = 0;   // esp_now_send returned != OK

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
#else
static void onRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
#endif
    statRaw++;
    if (len < (int)sizeof(MeshEvent)) { statBadMagic++; return; }
    const MeshEvent* p = (const MeshEvent*)data;
    if (p->magic0 != 'P' || p->magic1 != 'M') { statBadMagic++; return; }
    if (p->msgType != MESH_MSG_EVENT)         { statBadMagic++; return; }

    uint8_t nh = (rxHead + 1) & 7;
    if (nh == rxTail) { statRingFull++; return; }  // ring full — drop
    rx[rxHead] = *p;
    rxHead = nh;
}

inline void handle(const MeshEvent& ev) {
    uint32_t key = keyOf(ev.originId, ev.seq);
    if (wasSeen(key)) return;
    markSeen(key);

    bool relayed = ev.ttl < MESH_TTL_START;
    if (eventCb) eventCb(ev, relayed);

    if (ev.ttl > 0) {
        MeshEvent r = ev;
        r.ttl--;
        enqueue(r, 1, 0, (uint16_t)random(3, 28));  // small jitter vs collisions
    }
}

}  // namespace _mesh

// ── Public API ─────────────────────────────────────────────────────────────
inline bool mesh_init(uint8_t originType, uint16_t originId, uint8_t wifiChannel = MESH_WIFI_CHANNEL) {
    _mesh::originType = originType;
    _mesh::originId   = originId;
    memset(_mesh::seen, 0, sizeof(_mesh::seen));
    memset((void*)_mesh::out, 0, sizeof(_mesh::out));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("  [MESH] esp_now_init FAILED");
        return false;
    }
    esp_now_register_recv_cb(_mesh::onRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, _mesh::BCAST, 6);
    peer.channel = 0;        // 0 = use current channel
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // Read the channel back — asking for one is not the same as getting it,
    // and a silent mismatch makes the mesh look dead rather than misconfigured.
    uint8_t actualCh = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&actualCh, &sec);

    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    Serial.printf("  [MESH] up — origin 0x%02X id 0x%04X  ch %d (asked %d)  mac %02X:%02X:%02X:%02X:%02X:%02X\n",
                  originType, originId, actualCh, wifiChannel,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (actualCh != wifiChannel)
        Serial.printf("  [MESH] !! CHANNEL MISMATCH — wanted %d, radio is on %d\n", wifiChannel, actualCh);
    return true;
}

// Print radio counters. Call from a periodic log to see where packets go.
inline void mesh_print_stats() {
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);
    Serial.printf("  [MESH] ch=%d sent=%lu sendErr=%lu rawRx=%lu badMagic=%lu ringFull=%lu\n",
                  ch, (unsigned long)_mesh::statSent, (unsigned long)_mesh::statSendErr,
                  (unsigned long)_mesh::statRaw, (unsigned long)_mesh::statBadMagic,
                  (unsigned long)_mesh::statRingFull);
}

inline void mesh_on_event(MeshEventCb cb) { _mesh::eventCb = cb; }

inline void mesh_send_event(uint8_t channel, uint8_t mode, uint8_t magnitude, uint8_t flags) {
    MeshEvent ev;
    ev.magic0     = 'P';
    ev.magic1     = 'M';
    ev.msgType    = MESH_MSG_EVENT;
    ev.originType = _mesh::originType;
    ev.originId   = _mesh::originId;
    ev.seq        = ++_mesh::seqCounter;
    ev.ttl        = MESH_TTL_START;
    ev.channel    = channel;
    ev.mode       = mode;
    ev.magnitude  = magnitude;
    ev.flags      = flags;
    ev.reserved   = 0;

    _mesh::markSeen(_mesh::keyOf(ev.originId, ev.seq));   // don't relay our own echo
    _mesh::enqueue(ev, MESH_BURST, MESH_BURST_GAP, 0);
}

inline void mesh_poll() {
    // 1. drain received packets (dedupe + user callback + relay enqueue)
    while (_mesh::rxTail != _mesh::rxHead) {
        MeshEvent ev = _mesh::rx[_mesh::rxTail];
        _mesh::rxTail = (_mesh::rxTail + 1) & 7;
        _mesh::handle(ev);
    }
    // 2. service the outbound queue (local bursts + relays)
    uint32_t now = millis();
    for (uint8_t i = 0; i < 16; i++) {
        _mesh::OutSlot& s = _mesh::out[i];
        if (!s.used || now < s.dueMs) continue;
        esp_err_t e = esp_now_send(_mesh::BCAST, (const uint8_t*)&s.ev, sizeof(MeshEvent));
        _mesh::statSent++;
        if (e != ESP_OK) _mesh::statSendErr++;
        if (--s.left == 0) s.used = false;
        else s.dueMs = now + s.gapMs;
    }
}

}  // namespace pulleys
