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
    MESH_ORIGIN_SENSOR  = 0x04,
    MESH_ORIGIN_SCREEN  = 0x05,
    MESH_ORIGIN_ARBITER = 0x06,   // monitor; joins the mesh but shows no art
    MESH_ORIGIN_BRIDGE  = 0x07,   // forwards mesh events to a Pi over serial
};

enum : uint8_t {
    MESH_MSG_EVENT  = 0x10,
    MESH_MSG_SYNC   = 0x20,   // shared clock beacon
    MESH_MSG_DIGEST = 0x30,   // reserved
    MESH_MSG_OTA_ANNOUNCE = 0x40,   // host: "my AP is up, this image is for you"
    MESH_MSG_OTA_READY    = 0x41,   // node: "joined, here is my address"
    MESH_MSG_HELLO        = 0x50,   // "everyone say what you are"
    MESH_MSG_CENSUS       = 0x51,   // the answer to that
};

// Clock sync: every node beacons its own mesh clock, and each listener pulls a
// fraction of the way toward what it hears. Consensus averaging, so there is no
// master to lose and a corrupt value washes out over the next few beacons
// instead of poisoning the field permanently. Precision is tens of ms — far
// finer than the 0.2–2 Hz oscillation it exists to keep in step.
static constexpr uint32_t MESH_SYNC_INTERVAL_MS = 2000;
static constexpr float    MESH_SYNC_GAIN        = 0.25f;
static constexpr int32_t  MESH_SYNC_MAX_STEP_MS = 400;   // gentle slew once locked

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

struct __attribute__((packed)) MeshSync {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_SYNC
    uint8_t  originType;  // MESH_ORIGIN_*
    uint32_t meshNow;     // sender's mesh clock, ms
    uint16_t originId;    // appended after meshNow — older nodes ignore the tail
};  // 10 bytes

// ── OTA control ──────────────────────────────────────────────────────────────
// Firmware never travels over ESP-NOW — only the invitation does. The host
// raises a SoftAP on this same channel and repeats an announce; a node that
// recognises its own env joins the AP and pulls the image over HTTP.
//
// Repeated rather than flooded: these are not events, so they skip the dedupe
// ring and the relay entirely. A node that misses one hears the next, and a
// node that boots mid-window still gets invited.
struct __attribute__((packed)) MeshOtaAnnounce {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_OTA_ANNOUNCE
    uint8_t  originType;  // MESH_ORIGIN_* of the host
    uint16_t originId;    // host's device ID
    char     ssid[32];    // the host's SoftAP
    char     pass[16];
    char     env[16];     // PlatformIO env this image is for — a node whose own
                          // PULLEYS_ENV differs stays in the art and ignores it
    uint16_t idFilter;    // 0 = every board of that env; else only this device ID
    uint16_t windowSecs;  // how long the host will keep serving
    char     md5[33];     // md5 of the image on offer. A node remembers what it
                          // last took, so it can tell "there is an update" from
                          // "the invitation I already accepted is still being
                          // repeated" -- without this a node reboots into the
                          // new image, hears the same announce, and takes it
                          // again, forever.
};  // 109 bytes

struct __attribute__((packed)) MeshOtaReady {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_OTA_READY
    uint8_t  originType;  // MESH_ORIGIN_* of the node checking in
    uint16_t originId;    // node's device ID
    char     env[16];     // what the node is running now
    uint8_t  ip[4];       // its address on the host's AP
    uint8_t  state;       // OTA_STATE_*
    int16_t  err;         // updater error code when state is FAILED; else 0.
                          // A node up a rope has no USB port to be debugged
                          // through, so the reason has to come back over the
                          // air or it is not available at all.
};  // 30 bytes

enum : uint8_t {
    OTA_STATE_READY       = 0,   // joined the AP, about to pull
    OTA_STATE_DOWNLOADING = 1,
    OTA_STATE_OK          = 2,   // written; rebooting into it
    OTA_STATE_FAILED      = 3,
};

// ── Census ───────────────────────────────────────────────────────────────────
// The clock beacon already proves a board is alive, but it carries only an ID
// and a type -- not what firmware that board is running, which is the question
// that matters once boards are reflashed without a cable. HELLO asks; every
// board answers with a CENSUS naming itself.
//
// Replies are spread over a window the asker names, because a dozen boards
// answering the same instant is a dozen boards colliding.
struct __attribute__((packed)) MeshHello {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_HELLO
    uint8_t  originType;  // MESH_ORIGIN_* of the asker
    uint16_t originId;
    uint16_t nonce;       // ties answers to the round that asked for them
    uint16_t spreadMs;    // answer at a random point inside this window
};  // 10 bytes

struct __attribute__((packed)) MeshCensus {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_CENSUS
    uint8_t  originType;  // MESH_ORIGIN_*
    uint16_t originId;
    uint16_t nonce;       // echoed from the HELLO
    char     env[16];     // the PlatformIO env this board is running
    char     build[24];   // its compile stamp -- the same token whoami prints
    char     detail[16];  // whatever the role thinks identifies it in a list:
                          // a sensor's rope and mode, a screen's display
    uint32_t uptimeSecs;
};  // 70 bytes

// Every node beacons, so this is also the presence signal for roles that emit
// no events of their own: without it a Screen is invisible to a monitor.
typedef void (*MeshEventCb)(const MeshEvent& ev, bool relayed);
typedef void (*MeshSyncCb)(uint8_t originType, uint16_t originId, int32_t skewMs);
typedef void (*MeshOtaAnnounceCb)(const MeshOtaAnnounce& a);
typedef void (*MeshOtaReadyCb)(const MeshOtaReady& r);
typedef void (*MeshHelloCb)(const MeshHello& h);
typedef void (*MeshCensusCb)(const MeshCensus& c);

// ── Internal state ──────────────────────────────────────────────────────────
namespace _mesh {

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Outbound queue slot — handles both local bursts and relay resends.
struct OutSlot {
    MeshEvent ev;
    uint32_t  dueMs;
    uint16_t  gapMs;
    uint8_t   left;
    bool      used;
};

// All mutable state lives in one struct reached through an inline accessor.
// This header is included by several translation units in the arbiter, and
// namespace-scope `static` would give each of them a private copy: one file
// would register the callbacks while another polled its own empty rings.
// A function-local static inside an inline function is one shared instance
// across the whole program, guaranteed even under C++11.
struct State {
    uint8_t     originType = 0;
    uint16_t    originId   = 0;
    uint16_t    seqCounter = 0;
    MeshEventCb eventCb    = nullptr;
    MeshSyncCb  syncCb     = nullptr;
    MeshOtaAnnounceCb otaAnnounceCb = nullptr;
    MeshOtaReadyCb    otaReadyCb    = nullptr;
    MeshHelloCb       helloCb       = nullptr;
    MeshCensusCb      censusCb      = nullptr;
    bool        relayOn    = true;

    // Deduplication ring — linear scan, fine at this node count / event rate.
    uint32_t seen[512]  = {};
    uint16_t seenHead   = 0;

    OutSlot out[16]     = {};

    // RX ring — the WiFi task is the single producer (a real FreeRTOS task, not
    // an ISR), mesh_poll() the single consumer, so volatile indices around a
    // plain slot array are sufficient.
    MeshEvent          rx[8]  = {};
    volatile uint8_t   rxHead = 0;
    volatile uint8_t   rxTail = 0;

    // Clock sync. clockOffset is added to millis() to get the mesh clock.
    int32_t  clockOffset  = 0;
    bool     clockLocked  = false;
    uint32_t lastSyncTx   = 0;
    uint32_t syncInterval = MESH_SYNC_INTERVAL_MS;
    MeshSync syncRx[4]    = {};
    volatile uint8_t syncHead = 0;
    volatile uint8_t syncTail = 0;

    // OTA control rings. The announce repeats, so one slot is enough — a
    // missed copy costs nothing. Check-ins come from every node at once, so
    // those get a ring deep enough for a crate of boards answering together.
    MeshOtaAnnounce otaRx     = {};
    volatile bool   otaRxFull = false;
    MeshOtaReady    readyRx[8] = {};
    volatile uint8_t readyHead = 0;
    volatile uint8_t readyTail = 0;

    // Census. The reply ring is deep enough for every board in the piece to
    // answer one round before anything is drained.
    MeshHello       helloRx    = {};
    volatile bool   helloRxFull = false;
    MeshCensus      censusRx[16] = {};
    volatile uint8_t censusHead = 0;
    volatile uint8_t censusTail = 0;

    // Diagnostics — every frame the radio hands us, before and after filtering.
    volatile uint32_t statRaw      = 0;
    volatile uint32_t statBadMagic = 0;
    volatile uint32_t statRingFull = 0;
    uint32_t          statSent     = 0;
    uint32_t          statSendErr  = 0;
};

inline State& S() { static State s; return s; }

inline bool wasSeen(uint32_t key) {
    State& s = S();
    for (uint16_t i = 0; i < 512; i++) if (s.seen[i] == key) return true;
    return false;
}
inline void markSeen(uint32_t key) {
    State& s = S();
    s.seen[s.seenHead] = key;
    s.seenHead = (s.seenHead + 1) & 511;
}
inline uint32_t keyOf(uint16_t id, uint16_t sq) {
    return ((uint32_t)id << 16) | sq;
}

inline void enqueue(const MeshEvent& ev, uint8_t count, uint16_t gap, uint16_t firstDelay) {
    State& s = S();
    for (uint8_t i = 0; i < 16; i++) {
        if (s.out[i].used) continue;
        s.out[i].ev    = ev;
        s.out[i].left  = count;
        s.out[i].gapMs = gap;
        s.out[i].dueMs = millis() + firstDelay;
        s.out[i].used  = true;
        return;
    }
    // queue full — drop silently (bursts make this non-fatal)
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
#else
static void onRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
#endif
    State& st = S();
    st.statRaw++;
    if (len < (int)sizeof(MeshSync)) { st.statBadMagic++; return; }
    if (data[0] != 'P' || data[1] != 'M') { st.statBadMagic++; return; }

    if (data[2] == MESH_MSG_SYNC) {
        uint8_t nh = (st.syncHead + 1) & 3;
        if (nh != st.syncTail) {
            memcpy(&st.syncRx[st.syncHead], data, sizeof(MeshSync));
            st.syncHead = nh;
        }
        return;
    }

    if (data[2] == MESH_MSG_OTA_ANNOUNCE) {
        if (len < (int)sizeof(MeshOtaAnnounce)) { st.statBadMagic++; return; }
        memcpy((void*)&st.otaRx, data, sizeof(MeshOtaAnnounce));
        st.otaRxFull = true;      // latest wins; the announce repeats anyway
        return;
    }

    if (data[2] == MESH_MSG_OTA_READY) {
        if (len < (int)sizeof(MeshOtaReady)) { st.statBadMagic++; return; }
        uint8_t rh = (st.readyHead + 1) & 7;
        if (rh != st.readyTail) {
            memcpy(&st.readyRx[st.readyHead], data, sizeof(MeshOtaReady));
            st.readyHead = rh;
        }
        return;
    }

    if (data[2] == MESH_MSG_HELLO) {
        if (len < (int)sizeof(MeshHello)) { st.statBadMagic++; return; }
        memcpy((void*)&st.helloRx, data, sizeof(MeshHello));
        st.helloRxFull = true;
        return;
    }

    if (data[2] == MESH_MSG_CENSUS) {
        if (len < (int)sizeof(MeshCensus)) { st.statBadMagic++; return; }
        uint8_t ch = (st.censusHead + 1) & 15;
        if (ch != st.censusTail) {
            memcpy(&st.censusRx[st.censusHead], data, sizeof(MeshCensus));
            st.censusHead = ch;
        }
        return;
    }

    if (len < (int)sizeof(MeshEvent)) { st.statBadMagic++; return; }
    const MeshEvent* p = (const MeshEvent*)data;
    if (p->msgType != MESH_MSG_EVENT) { st.statBadMagic++; return; }

    uint8_t nh = (st.rxHead + 1) & 7;
    if (nh == st.rxTail) { st.statRingFull++; return; }  // ring full — drop
    st.rx[st.rxHead] = *p;
    st.rxHead = nh;
}

inline void handle(const MeshEvent& ev) {
    uint32_t key = keyOf(ev.originId, ev.seq);
    if (wasSeen(key)) return;
    markSeen(key);

    bool relayed = ev.ttl < MESH_TTL_START;
    if (S().eventCb) S().eventCb(ev, relayed);

    if (S().relayOn && ev.ttl > 0) {
        MeshEvent r = ev;
        r.ttl--;
        enqueue(r, 1, 0, (uint16_t)random(3, 28));  // small jitter vs collisions
    }
}

}  // namespace _mesh

// ── Public API ─────────────────────────────────────────────────────────────
inline bool mesh_init(uint8_t originType, uint16_t originId, uint8_t wifiChannel = MESH_WIFI_CHANNEL) {
    _mesh::State& st = _mesh::S();
    st.originType = originType;
    st.originId   = originId;
    memset(st.seen, 0, sizeof(st.seen));
    memset((void*)st.out, 0, sizeof(st.out));

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
                  ch, (unsigned long)_mesh::S().statSent, (unsigned long)_mesh::S().statSendErr,
                  (unsigned long)_mesh::S().statRaw, (unsigned long)_mesh::S().statBadMagic,
                  (unsigned long)_mesh::S().statRingFull);
}

inline void mesh_on_event(MeshEventCb cb) { _mesh::S().eventCb = cb; }

// Observe every clock beacon: node presence plus that node's skew from ours.
inline void mesh_on_sync(MeshSyncCb cb) { _mesh::S().syncCb = cb; }

// OTA control. The host subscribes to check-ins, every other role to announces.
inline void mesh_on_ota_announce(MeshOtaAnnounceCb cb) { _mesh::S().otaAnnounceCb = cb; }
inline void mesh_on_ota_ready(MeshOtaReadyCb cb)       { _mesh::S().otaReadyCb    = cb; }

// Sent straight out rather than through the burst queue: the host repeats the
// announce for the whole window, so retry is built into the caller's cadence.
inline void mesh_send_ota_announce(const char* ssid, const char* pass, const char* env,
                                   uint16_t idFilter, uint16_t windowSecs,
                                   const char* md5) {
    MeshOtaAnnounce a = {};
    a.magic0     = 'P';
    a.magic1     = 'M';
    a.msgType    = MESH_MSG_OTA_ANNOUNCE;
    a.originType = _mesh::S().originType;
    a.originId   = _mesh::S().originId;
    strncpy(a.ssid, ssid, sizeof(a.ssid) - 1);
    strncpy(a.pass, pass, sizeof(a.pass) - 1);
    strncpy(a.env,  env,  sizeof(a.env)  - 1);
    a.idFilter   = idFilter;
    a.windowSecs = windowSecs;
    if (md5) strncpy(a.md5, md5, sizeof(a.md5) - 1);
    esp_now_send(_mesh::BCAST, (const uint8_t*)&a, sizeof(a));
}

inline void mesh_send_ota_ready(const char* env, const uint8_t ip[4], uint8_t state,
                                int16_t err = 0) {
    MeshOtaReady r = {};
    r.magic0     = 'P';
    r.magic1     = 'M';
    r.msgType    = MESH_MSG_OTA_READY;
    r.originType = _mesh::S().originType;
    r.originId   = _mesh::S().originId;
    strncpy(r.env, env, sizeof(r.env) - 1);
    if (ip) memcpy(r.ip, ip, 4);
    r.state = state;
    r.err   = err;
    esp_now_send(_mesh::BCAST, (const uint8_t*)&r, sizeof(r));
}

// Census. Every role subscribes to HELLO; whoever asked subscribes to CENSUS.
inline void mesh_on_hello(MeshHelloCb cb)   { _mesh::S().helloCb  = cb; }
inline void mesh_on_census(MeshCensusCb cb) { _mesh::S().censusCb = cb; }

inline uint16_t mesh_send_hello(uint16_t spreadMs) {
    MeshHello h = {};
    h.magic0     = 'P';
    h.magic1     = 'M';
    h.msgType    = MESH_MSG_HELLO;
    h.originType = _mesh::S().originType;
    h.originId   = _mesh::S().originId;
    h.nonce      = (uint16_t)random(1, 65535);
    h.spreadMs   = spreadMs;
    esp_now_send(_mesh::BCAST, (const uint8_t*)&h, sizeof(h));
    return h.nonce;
}

inline void mesh_send_census(uint16_t nonce, const char* env, const char* build,
                             const char* detail, uint32_t uptimeSecs) {
    MeshCensus c = {};
    c.magic0     = 'P';
    c.magic1     = 'M';
    c.msgType    = MESH_MSG_CENSUS;
    c.originType = _mesh::S().originType;
    c.originId   = _mesh::S().originId;
    c.nonce      = nonce;
    if (env)    strncpy(c.env,    env,    sizeof(c.env)    - 1);
    if (build)  strncpy(c.build,  build,  sizeof(c.build)  - 1);
    if (detail) strncpy(c.detail, detail, sizeof(c.detail) - 1);
    c.uptimeSecs = uptimeSecs;
    esp_now_send(_mesh::BCAST, (const uint8_t*)&c, sizeof(c));
}

// Turn off rebroadcasting to observe the field without altering it. A monitor
// that relays is a legitimate extra hop and helps coverage, but it also masks
// the very range gaps you might be trying to find.
inline void mesh_set_relay(bool on) { _mesh::S().relayOn = on; }

// ── Shared clock ─────────────────────────────────────────────────────────────
// Drive every time-based animation from this instead of millis(), and the whole
// field oscillates in step. Monotonic in practice: the only backward motion is
// a bounded slew of at most MESH_SYNC_MAX_STEP_MS per beacon.
inline uint32_t mesh_now()      { return millis() + _mesh::S().clockOffset; }
inline float    mesh_now_secs() { return mesh_now() / 1000.0f; }
inline bool     mesh_clock_locked() { return _mesh::S().clockLocked; }

inline void mesh_send_event(uint8_t channel, uint8_t mode, uint8_t magnitude, uint8_t flags) {
    MeshEvent ev;
    ev.magic0     = 'P';
    ev.magic1     = 'M';
    ev.msgType    = MESH_MSG_EVENT;
    _mesh::State& st = _mesh::S();
    ev.originType = st.originType;
    ev.originId   = st.originId;
    ev.seq        = ++st.seqCounter;
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
    // 0. clock sync — adopt the first peer heard outright, then slew gently
    _mesh::State& st = _mesh::S();
    while (st.syncTail != st.syncHead) {
        MeshSync beacon = st.syncRx[st.syncTail];
        st.syncTail = (st.syncTail + 1) & 3;

        int32_t delta = (int32_t)(beacon.meshNow - mesh_now());   // wrap-safe
        if (st.syncCb) st.syncCb(beacon.originType, beacon.originId, delta);
        if (!st.clockLocked) {
            st.clockOffset += delta;                      // step straight onto the field
            st.clockLocked  = true;
            Serial.printf("  [MESH] clock locked (stepped %ld ms)\n", (long)delta);
        } else {
            int32_t step = (int32_t)(delta * MESH_SYNC_GAIN);
            if (step >  MESH_SYNC_MAX_STEP_MS) step =  MESH_SYNC_MAX_STEP_MS;
            if (step < -MESH_SYNC_MAX_STEP_MS) step = -MESH_SYNC_MAX_STEP_MS;
            st.clockOffset += step;
        }
    }

    // Beacon our own clock. The jitter goes into the next interval, never into
    // the timestamp: scheduling lastSyncTx in the future makes the unsigned
    // (nowMs - lastSyncTx) underflow, which fires the beacon every poll.
    uint32_t nowMs = millis();
    if (nowMs - st.lastSyncTx >= st.syncInterval) {
        st.lastSyncTx   = nowMs;
        st.syncInterval = MESH_SYNC_INTERVAL_MS + (uint32_t)random(0, 400);
        MeshSync s;
        s.magic0     = 'P';
        s.magic1     = 'M';
        s.msgType    = MESH_MSG_SYNC;
        s.originType = st.originType;
        s.meshNow    = mesh_now();
        s.originId   = st.originId;
        esp_now_send(_mesh::BCAST, (const uint8_t*)&s, sizeof(s));
    }

    // 0b. OTA control — delivered straight to the callback, no dedupe, no relay
    if (st.otaRxFull) {
        MeshOtaAnnounce a = (MeshOtaAnnounce&)st.otaRx;
        st.otaRxFull = false;
        if (st.otaAnnounceCb) st.otaAnnounceCb(a);
    }
    while (st.readyTail != st.readyHead) {
        MeshOtaReady r = st.readyRx[st.readyTail];
        st.readyTail = (st.readyTail + 1) & 7;
        if (st.otaReadyCb) st.otaReadyCb(r);
    }
    if (st.helloRxFull) {
        MeshHello h = (MeshHello&)st.helloRx;
        st.helloRxFull = false;
        if (st.helloCb) st.helloCb(h);
    }
    while (st.censusTail != st.censusHead) {
        MeshCensus c = st.censusRx[st.censusTail];
        st.censusTail = (st.censusTail + 1) & 15;
        if (st.censusCb) st.censusCb(c);
    }

    // 1. drain received packets (dedupe + user callback + relay enqueue)
    while (st.rxTail != st.rxHead) {
        MeshEvent ev = st.rx[st.rxTail];
        st.rxTail = (st.rxTail + 1) & 7;
        _mesh::handle(ev);
    }
    // 2. service the outbound queue (local bursts + relays)
    uint32_t now = millis();
    for (uint8_t i = 0; i < 16; i++) {
        _mesh::OutSlot& s = st.out[i];
        if (!s.used || now < s.dueMs) continue;
        esp_err_t e = esp_now_send(_mesh::BCAST, (const uint8_t*)&s.ev, sizeof(MeshEvent));
        st.statSent++;
        if (e != ESP_OK) st.statSendErr++;
        if (--s.left == 0) s.used = false;
        else s.dueMs = now + s.gapMs;
    }
}

}  // namespace pulleys
