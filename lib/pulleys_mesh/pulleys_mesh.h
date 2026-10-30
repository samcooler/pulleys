#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <pulleys_install.h>   // CHANNEL_COUNT

// ── pulleys_mesh — connectionless ESP-NOW broadcast with a flood relay ────────
//
// Every node broadcasts on a fixed Wi-Fi channel. Sensors emit small EVENT
// packets; all nodes dedupe by (originId, seq), act once, and rebroadcast unseen
// events with a decrementing TTL. No AP, no pairing, no routing state.
//
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
    MESH_ORIGIN_COMPONENT_STATE_DISPLAY   = 0x08,   // shows the components' charge on an LED matrix
};

enum : uint8_t {
    MESH_MSG_EVENT  = 0x10,
    MESH_MSG_SYNC   = 0x20,   // shared clock beacon
    MESH_MSG_DIGEST = 0x30,   // reserved
    MESH_MSG_OTA_ANNOUNCE = 0x40, // compatibility invitation for deployed nodes
    MESH_MSG_OTA_READY    = 0x41, // compatibility status from legacy updater
    MESH_MSG_HELLO        = 0x50,   // "everyone say what you are"
    MESH_MSG_CENSUS       = 0x51,   // the answer to that
    MESH_MSG_CUE          = 0x60,   // the arbiter's game fired a cue
    MESH_MSG_COMPONENTS   = 0x70,   // the arbiter's view of every component's charge
    MESH_MSG_FW_OFFER     = 0x80,   // bridge: targeted firmware update offer
    MESH_MSG_FW_STATUS    = 0x81,   // node: update progress/result
    MESH_MSG_CONFIG       = 0x90,   // arbiter: sensor -> channel map, channel -> colour
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
    uint8_t  channel;     // 0..CHANNEL_COUNT-1
    uint8_t  mode;        // SENSOR_MODE_*
    uint8_t  magnitude;   // quantized strength (degrees/2 or impulse level)
    uint8_t  flags;       // MESH_EVENT_FLAG_*
    uint8_t  reserved;
};  // 14 bytes

enum : uint8_t {
    MESH_EVENT_FLAG_BATTERY_LOW = 0x01,
    // Not a pull: the sensor announcing the channel it has just been moved to,
    // so the arbiter can show a remap as confirmed without anyone pulling the
    // rope again. Anything that counts detections must skip these.
    MESH_EVENT_FLAG_ACK         = 0x02,
};
inline bool mesh_event_is_pull(const MeshEvent& ev) {
    return !(ev.flags & MESH_EVENT_FLAG_ACK);
}

struct __attribute__((packed)) MeshSync {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_SYNC
    uint8_t  originType;  // MESH_ORIGIN_*
    uint32_t meshNow;     // sender's mesh clock, ms
    uint16_t originId;    // appended after meshNow — older nodes ignore the tail
};  // 10 bytes

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
    char     env[32];     // the PlatformIO env this board is running
    char     build[24];   // its compile stamp -- the same token whoami prints
    char     detail[16];  // whatever the role thinks identifies it in a list:
                          // a sensor's rope and mode, a screen's display
    uint32_t uptimeSecs;
    char     fwHash[65];  // release build identity SHA-256
};  // 149 bytes

struct __attribute__((packed)) MeshCensusV1 {
    uint8_t magic0, magic1, msgType, originType;
    uint16_t originId, nonce;
    char env[16], build[24], detail[16];
    uint32_t uptimeSecs;
};

struct __attribute__((packed)) MeshFwOffer {
    uint8_t  magic0, magic1, msgType, originType;
    uint16_t originId, targetId;
    char     env[32];
    char     buildHash[65];
    char     sha256[65];
    uint32_t size;
};

// Compatibility wire messages used while migrating nodes on the old OTA client.
struct __attribute__((packed)) MeshOtaAnnounce {
    uint8_t magic0, magic1, msgType, originType;
    uint16_t originId;
    char ssid[32], pass[16], env[16];
    uint16_t idFilter, windowSecs;
    char md5[33];
};
struct __attribute__((packed)) MeshOtaReady {
    uint8_t magic0, magic1, msgType, originType;
    uint16_t originId;
    char env[32];
    uint8_t ip[4], state;
    int16_t err;
};
enum : uint8_t { OTA_STATE_READY = 0, OTA_STATE_DOWNLOADING = 1,
                 OTA_STATE_OK = 2, OTA_STATE_FAILED = 3 };

struct __attribute__((packed)) MeshFwStatus {
    uint8_t magic0, magic1, msgType, originType;
    uint16_t originId;
    char env[32];
    uint8_t state; // 0=ready, 1=downloading, 2=installed/rebooting, 3=failed
    int16_t error;
    char buildHash[65];
    char sha256[65];
};

// ── Cue ──────────────────────────────────────────────────────────────────────
// The arbiter's game state machine fires cues (a sound, a clip) for the Pi to
// play. A cue travels as its name, ASCII: "audio_once_chime". The packet is as
// long as the name needs, not a fixed size -- nameLen says how many bytes of
// name follow the header, and a receiver that got fewer (a relay running older
// firmware that copies a fixed length would cut it short) drops the packet
// rather than act on half a name.
//
// Flooded and deduped exactly like an EVENT -- same (originId, seq) key, same
// TTL and burst -- because a lost cue is a missed sound.
static constexpr uint8_t MESH_CUE_NAME_MAX = 47;

struct __attribute__((packed)) MeshCue {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_CUE
    uint8_t  originType;  // MESH_ORIGIN_*
    uint16_t originId;
    uint16_t seq;         // shares the origin's counter with its events
    uint8_t  ttl;         // remaining flood hops
    uint8_t  nameLen;     // bytes of name that follow, 1..MESH_CUE_NAME_MAX
    char     name[MESH_CUE_NAME_MAX + 1];   // NUL-terminated here; not sent on the wire
};  // 10 bytes of header, then nameLen bytes of name

static constexpr size_t MESH_CUE_HEADER = offsetof(MeshCue, name);
inline size_t mesh_cue_wire_size(const MeshCue& c) { return MESH_CUE_HEADER + c.nameLen; }

// ── Components ───────────────────────────────────────────────────────────────
// How charged each game component is, as the arbiter's mapper sees it, for a
// component state display to draw. The arbiter owns the rules for when a component is lit and for
// how long; this only publishes the result, so the display never repeats them.
//
// Index order is fixed: majors 1-5, then finals 1-3. charge is a percentage,
// 100 the moment a component is lit and 0 once it is dark, and `decay` is how
// fast it drains, so a receiver can keep draining on its own between packets.
// The arbiter repeats the packet about once a second, which also corrects a
// receiver that missed one.
//
// Flooded and deduped like a cue, so a display out of the arbiter's direct range
// still gets it.
static constexpr uint8_t MESH_COMPONENT_COUNT = 8;

struct __attribute__((packed)) MeshComponents {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_COMPONENTS
    uint8_t  originType;  // MESH_ORIGIN_*
    uint16_t originId;
    uint16_t seq;         // shares the origin's counter with its events
    uint8_t  ttl;         // remaining flood hops
    uint16_t decay;       // drain rate, in thousandths of a percent per second
    uint8_t  charge[MESH_COMPONENT_COUNT];   // percent: 0 = dark .. 100 = just lit
    // Appended: the colour (pulleys_channel COLOR_*) each component wears today,
    // so a display draws a bar in its ropes' colour. A packet from an arbiter
    // that predates it stops at charge[]; the receiver fills this with
    // MESH_COLOR_UNKNOWN.
    uint8_t  color[MESH_COMPONENT_COUNT];
};  // 27 bytes
static constexpr size_t  MESH_COMPONENTS_V1_SIZE = offsetof(MeshComponents, color);
static constexpr uint8_t MESH_COLOR_UNKNOWN      = 0xFF;

// ── Config ───────────────────────────────────────────────────────────────────
// The install's two arbiter-owned tables, whole, in one packet: which channel
// each sensor is on, and which colour each channel wears (an index into
// pulleys_channel's COLOR_*). The arbiter sends a burst on every change and
// repeats it every few seconds, so a board that missed one, rebooted or lost
// its NVS converges on the next repeat with no acks and no retries.
//
// Receivers compare against what they have and act only on a difference, so
// the repeat costs nothing. A sensor the map does not name keeps the channel it
// has: an arbiter that boots with an empty map must never strand the field.
//
// Variable length like a cue: `count` rows follow the header, and a packet
// shorter than that claims is dropped.
static constexpr uint8_t MESH_CONFIG_MAX_ROWS = 64;

struct __attribute__((packed)) MeshConfigRow {
    uint16_t id;          // sensor device ID, as identity_id()
    uint8_t  channel;
};

struct __attribute__((packed)) MeshConfig {
    uint8_t  magic0;      // 'P'
    uint8_t  magic1;      // 'M'
    uint8_t  msgType;     // MESH_MSG_CONFIG
    uint8_t  originType;  // MESH_ORIGIN_*
    uint16_t originId;
    uint16_t seq;         // shares the origin's counter with its events
    uint8_t  ttl;         // remaining flood hops
    uint8_t  colors[CHANNEL_COUNT];   // channel -> COLOR_*
    uint8_t  count;       // rows that follow
    MeshConfigRow rows[MESH_CONFIG_MAX_ROWS];
};
static constexpr size_t MESH_CONFIG_HEADER = offsetof(MeshConfig, rows);
static_assert(sizeof(MeshConfig) <= 250, "ESP-NOW frames carry at most 250 bytes");
inline size_t mesh_config_wire_size(const MeshConfig& c) {
    return MESH_CONFIG_HEADER + (size_t)c.count * sizeof(MeshConfigRow);
}

// The channel the map gives `id`, or -1 if it does not name it.
inline int mesh_config_channel_of(const MeshConfig& c, uint16_t id) {
    for (uint8_t i = 0; i < c.count && i < MESH_CONFIG_MAX_ROWS; i++)
        if (c.rows[i].id == id) return c.rows[i].channel;
    return -1;
}

// Every node beacons, so this is also the presence signal for roles that emit
// no events of their own: without it a Screen is invisible to a monitor.
typedef void (*MeshEventCb)(const MeshEvent& ev, bool relayed);
typedef void (*MeshSyncCb)(uint8_t originType, uint16_t originId, int32_t skewMs);
typedef void (*MeshOtaReadyCb)(const MeshOtaReady& status);
typedef void (*MeshHelloCb)(const MeshHello& h);
typedef void (*MeshCensusCb)(const MeshCensus& c);
typedef void (*MeshCueCb)(const MeshCue& c, bool relayed);
typedef void (*MeshComponentsCb)(const MeshComponents& c, bool relayed);
typedef void (*MeshConfigCb)(const MeshConfig& c, bool relayed);
typedef void (*MeshFwOfferCb)(const MeshFwOffer& offer);
typedef void (*MeshFwStatusCb)(const MeshFwStatus& status);

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
struct CueSlot {
    MeshCue  cue;
    uint32_t dueMs;
    uint16_t gapMs;
    uint8_t  left;
    bool     used;
};

struct ComponentsSlot {
    MeshComponents pkt;
    uint32_t       dueMs;
    uint16_t       gapMs;
    uint8_t        left;
    bool           used;
};

struct ConfigSlot {
    MeshConfig pkt;
    uint32_t   dueMs;
    uint16_t   gapMs;
    uint8_t    left;
    bool       used;
};

struct State {
    uint8_t     originType = 0;
    uint16_t    originId   = 0;
    uint16_t    seqCounter = 0;
    MeshEventCb eventCb    = nullptr;
    MeshSyncCb  syncCb     = nullptr;
    MeshOtaReadyCb otaReadyCb = nullptr;
    MeshHelloCb       helloCb       = nullptr;
    MeshCensusCb      censusCb      = nullptr;
    MeshCueCb         cueCb         = nullptr;
    MeshComponentsCb  componentsCb  = nullptr;
    MeshConfigCb      configCb      = nullptr;
    MeshFwOfferCb fwOfferCb = nullptr;
    MeshFwStatusCb fwStatusCb = nullptr;
    bool        relayOn    = true;

    // Deduplication ring — linear scan, fine at this node count / event rate.
    uint32_t seen[512]  = {};
    uint16_t seenHead   = 0;

    OutSlot out[16]     = {};
    CueSlot cueOut[8]   = {};
    ComponentsSlot compOut[4] = {};
    ConfigSlot     cfgOut[2]  = {};

    // Cue RX ring, same single-producer/single-consumer shape as the events'.
    MeshCue          cueRx[4] = {};
    volatile uint8_t cueHead  = 0;
    volatile uint8_t cueTail  = 0;

    // Components RX ring, same shape again.
    MeshComponents   compRx[4] = {};
    volatile uint8_t compHead  = 0;
    volatile uint8_t compTail  = 0;

    // Config RX ring. Two deep: it is large, and the next repeat replaces
    // anything dropped.
    MeshConfig       cfgRx[2] = {};
    volatile uint8_t cfgHead  = 0;
    volatile uint8_t cfgTail  = 0;

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

    // Census. The reply ring is deep enough for every board in the piece to
    // answer one round before anything is drained.
    MeshHello       helloRx    = {};
    volatile bool   helloRxFull = false;
    MeshCensus      censusRx[16] = {};
    volatile uint8_t censusHead = 0;
    volatile uint8_t censusTail = 0;
    MeshFwOffer fwOfferRx = {};
    volatile bool fwOfferFull = false;
    MeshFwStatus fwStatusRx[8] = {};
    volatile uint8_t fwStatusHead = 0, fwStatusTail = 0;
    MeshOtaReady otaReadyRx[8] = {};
    volatile uint8_t otaReadyHead = 0, otaReadyTail = 0;

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

inline void enqueueCue(const MeshCue& c, uint8_t count, uint16_t gap, uint16_t firstDelay) {
    State& s = S();
    for (uint8_t i = 0; i < 8; i++) {
        if (s.cueOut[i].used) continue;
        s.cueOut[i].cue   = c;
        s.cueOut[i].left  = count;
        s.cueOut[i].gapMs = gap;
        s.cueOut[i].dueMs = millis() + firstDelay;
        s.cueOut[i].used  = true;
        return;
    }
}

inline void enqueueComponents(const MeshComponents& c, uint8_t count, uint16_t gap, uint16_t firstDelay) {
    State& s = S();
    for (uint8_t i = 0; i < 4; i++) {
        if (s.compOut[i].used) continue;
        s.compOut[i].pkt   = c;
        s.compOut[i].left  = count;
        s.compOut[i].gapMs = gap;
        s.compOut[i].dueMs = millis() + firstDelay;
        s.compOut[i].used  = true;
        return;
    }
}

inline void enqueueConfig(const MeshConfig& c, uint8_t count, uint16_t gap, uint16_t firstDelay) {
    State& s = S();
    for (uint8_t i = 0; i < 2; i++) {
        if (s.cfgOut[i].used) continue;
        s.cfgOut[i].pkt   = c;
        s.cfgOut[i].left  = count;
        s.cfgOut[i].gapMs = gap;
        s.cfgOut[i].dueMs = millis() + firstDelay;
        s.cfgOut[i].used  = true;
        return;
    }
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

    if (data[2] == MESH_MSG_HELLO) {
        if (len < (int)sizeof(MeshHello)) { st.statBadMagic++; return; }
        memcpy((void*)&st.helloRx, data, sizeof(MeshHello));
        st.helloRxFull = true;
        return;
    }

    if (data[2] == MESH_MSG_CENSUS) {
        if (len < (int)sizeof(MeshCensusV1)) { st.statBadMagic++; return; }
        uint8_t ch = (st.censusHead + 1) & 15;
        if (ch != st.censusTail) {
            MeshCensus& slot = st.censusRx[st.censusHead];
            memset(&slot, 0, sizeof(slot));
            if (len >= (int)sizeof(MeshCensus)) {
                memcpy(&slot, data, sizeof(MeshCensus));
            } else {
                MeshCensusV1 old = {};
                memcpy(&old, data, sizeof(old));
                slot.magic0 = old.magic0; slot.magic1 = old.magic1;
                slot.msgType = old.msgType; slot.originType = old.originType;
                slot.originId = old.originId; slot.nonce = old.nonce;
                if (old.originType == MESH_ORIGIN_COMPONENT_STATE_DISPLAY &&
                    strncmp(old.env, "component_state", 15) == 0) {
                    strlcpy(slot.env, "component_state_display", sizeof(slot.env));
                } else {
                    memcpy(slot.env, old.env, sizeof(old.env));
                }
                memcpy(slot.build, old.build, sizeof(old.build));
                memcpy(slot.detail, old.detail, sizeof(old.detail));
                slot.uptimeSecs = old.uptimeSecs;
            }
            st.censusHead = ch;
        }
        return;
    }

    if (data[2] == MESH_MSG_OTA_READY) {
        if (len < (int)sizeof(MeshOtaReady)) { st.statBadMagic++; return; }
        uint8_t nh = (st.otaReadyHead + 1) & 7;
        if (nh != st.otaReadyTail) {
            memcpy(&st.otaReadyRx[st.otaReadyHead], data, sizeof(MeshOtaReady));
            st.otaReadyHead = nh;
        }
        return;
    }

    if (data[2] == MESH_MSG_FW_OFFER) {
        if (len < (int)sizeof(MeshFwOffer)) { st.statBadMagic++; return; }
        memcpy((void*)&st.fwOfferRx, data, sizeof(MeshFwOffer));
        st.fwOfferFull = true;
        return;
    }

    if (data[2] == MESH_MSG_FW_STATUS) {
        if (len < (int)sizeof(MeshFwStatus)) { st.statBadMagic++; return; }
        uint8_t nh = (st.fwStatusHead + 1) & 7;
        if (nh != st.fwStatusTail) {
            memcpy(&st.fwStatusRx[st.fwStatusHead], data, sizeof(MeshFwStatus));
            st.fwStatusHead = nh;
        }
        return;
    }

    if (data[2] == MESH_MSG_COMPONENTS) {
        if (len < (int)MESH_COMPONENTS_V1_SIZE) { st.statBadMagic++; return; }
        uint8_t ch = (st.compHead + 1) & 3;
        if (ch != st.compTail) {
            MeshComponents& slot = st.compRx[st.compHead];
            memset(slot.color, MESH_COLOR_UNKNOWN, sizeof(slot.color));
            memcpy(&slot, data, len < (int)sizeof(MeshComponents) ? (size_t)len : sizeof(MeshComponents));
            st.compHead = ch;
        }
        return;
    }

    if (data[2] == MESH_MSG_CONFIG) {
        if (len < (int)MESH_CONFIG_HEADER) { st.statBadMagic++; return; }
        uint8_t count = data[MESH_CONFIG_HEADER - 1];
        if (count > MESH_CONFIG_MAX_ROWS ||
            len < (int)(MESH_CONFIG_HEADER + count * sizeof(MeshConfigRow))) {
            st.statBadMagic++; return;
        }
        uint8_t ch = (st.cfgHead + 1) & 1;
        if (ch != st.cfgTail) {
            MeshConfig& slot = st.cfgRx[st.cfgHead];
            memset(&slot, 0, sizeof(slot));
            memcpy(&slot, data, MESH_CONFIG_HEADER + count * sizeof(MeshConfigRow));
            st.cfgHead = ch;
        }
        return;
    }

    if (data[2] == MESH_MSG_CUE) {
        if (len < (int)MESH_CUE_HEADER + 1) { st.statBadMagic++; return; }
        uint8_t nameLen = data[MESH_CUE_HEADER - 1];
        if (nameLen == 0 || nameLen > MESH_CUE_NAME_MAX ||
            len < (int)(MESH_CUE_HEADER + nameLen)) { st.statBadMagic++; return; }
        uint8_t ch = (st.cueHead + 1) & 3;
        if (ch != st.cueTail) {
            MeshCue& slot = st.cueRx[st.cueHead];
            memset(&slot, 0, sizeof(slot));
            memcpy(&slot, data, MESH_CUE_HEADER + nameLen);   // name stays NUL-terminated
            st.cueHead = ch;
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

inline void handleCue(const MeshCue& c) {
    uint32_t key = keyOf(c.originId, c.seq);
    if (wasSeen(key)) return;
    markSeen(key);

    bool relayed = c.ttl < MESH_TTL_START;
    if (S().cueCb) S().cueCb(c, relayed);

    if (S().relayOn && c.ttl > 0) {
        MeshCue r = c;
        r.ttl--;
        enqueueCue(r, 1, 0, (uint16_t)random(3, 28));
    }
}

inline void handleComponents(const MeshComponents& c) {
    uint32_t key = keyOf(c.originId, c.seq);
    if (wasSeen(key)) return;
    markSeen(key);

    bool relayed = c.ttl < MESH_TTL_START;
    if (S().componentsCb) S().componentsCb(c, relayed);

    if (S().relayOn && c.ttl > 0) {
        MeshComponents r = c;
        r.ttl--;
        enqueueComponents(r, 1, 0, (uint16_t)random(3, 28));
    }
}

inline void handleConfig(const MeshConfig& c) {
    uint32_t key = keyOf(c.originId, c.seq);
    if (wasSeen(key)) return;
    markSeen(key);

    bool relayed = c.ttl < MESH_TTL_START;
    if (S().configCb) S().configCb(c, relayed);

    if (S().relayOn && c.ttl > 0) {
        MeshConfig r = c;
        r.ttl--;
        enqueueConfig(r, 1, 0, (uint16_t)random(3, 28));
    }
}

}  // namespace _mesh

// ── Public API ─────────────────────────────────────────────────────────────
inline bool mesh_init(uint8_t originType, uint16_t originId, uint8_t wifiChannel = MESH_WIFI_CHANNEL) {
    _mesh::State& st = _mesh::S();
    st.originType = originType;
    st.originId   = originId;
    // Dedupe is keyed on (originId, seq). A node that reboots and counts from 1
    // again reuses keys its neighbours still remember, and its first packets
    // are dropped as duplicates until the ring turns over -- for the arbiter,
    // that is its opening cues. Start somewhere unrelated to the last boot.
    st.seqCounter = (uint16_t)esp_random();
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

// Census. Every role subscribes to HELLO; whoever asked subscribes to CENSUS.
inline void mesh_on_hello(MeshHelloCb cb)   { _mesh::S().helloCb  = cb; }
inline void mesh_on_census(MeshCensusCb cb) { _mesh::S().censusCb = cb; }

// Cues. Only the bridge needs to hear them; every role still relays them. The
// name in the callback's MeshCue is NUL-terminated and not otherwise checked.
inline void mesh_on_cue(MeshCueCb cb) { _mesh::S().cueCb = cb; }

// Components. The component state display subscribes; the arbiter sends. Every role relays them.
// `urgent` is a change (a component just lit or went dark), sent as a short
// burst; otherwise it is the periodic repeat, sent once.
inline void mesh_on_components(MeshComponentsCb cb) { _mesh::S().componentsCb = cb; }

inline void mesh_send_components(const uint8_t charge[MESH_COMPONENT_COUNT],
                                 const uint8_t color[MESH_COMPONENT_COUNT],
                                 uint16_t decay, bool urgent) {
    MeshComponents c = {};
    c.magic0     = 'P';
    c.magic1     = 'M';
    c.msgType    = MESH_MSG_COMPONENTS;
    _mesh::State& st = _mesh::S();
    c.originType = st.originType;
    c.originId   = st.originId;
    c.seq        = ++st.seqCounter;
    c.ttl        = MESH_TTL_START;
    c.decay      = decay;
    memcpy(c.charge, charge, MESH_COMPONENT_COUNT);
    memcpy(c.color,  color,  MESH_COMPONENT_COUNT);

    _mesh::markSeen(_mesh::keyOf(c.originId, c.seq));   // don't relay our own echo
    _mesh::enqueueComponents(c, urgent ? 3 : 1, 80, 0);
}

// Config. Sensors and Screens subscribe; the arbiter sends. Every role relays it.
// `urgent` is a change, sent as a burst; otherwise the periodic repeat, once.
inline void mesh_on_config(MeshConfigCb cb) { _mesh::S().configCb = cb; }

inline void mesh_send_config(const uint8_t colors[CHANNEL_COUNT],
                             const MeshConfigRow* rows, uint8_t count, bool urgent) {
    if (count > MESH_CONFIG_MAX_ROWS) count = MESH_CONFIG_MAX_ROWS;
    MeshConfig c = {};
    c.magic0     = 'P';
    c.magic1     = 'M';
    c.msgType    = MESH_MSG_CONFIG;
    _mesh::State& st = _mesh::S();
    c.originType = st.originType;
    c.originId   = st.originId;
    c.seq        = ++st.seqCounter;
    c.ttl        = MESH_TTL_START;
    memcpy(c.colors, colors, CHANNEL_COUNT);
    c.count      = count;
    memcpy(c.rows, rows, (size_t)count * sizeof(MeshConfigRow));

    _mesh::markSeen(_mesh::keyOf(c.originId, c.seq));   // don't relay our own echo
    _mesh::enqueueConfig(c, urgent ? MESH_BURST : 1, MESH_BURST_GAP, 0);
}

// False if the name is empty or too long to send.
inline bool mesh_send_cue(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > MESH_CUE_NAME_MAX) return false;
    MeshCue c = {};
    c.magic0     = 'P';
    c.magic1     = 'M';
    c.msgType    = MESH_MSG_CUE;
    _mesh::State& st = _mesh::S();
    c.originType = st.originType;
    c.originId   = st.originId;
    c.seq        = ++st.seqCounter;
    c.ttl        = MESH_TTL_START;
    c.nameLen    = (uint8_t)n;
    memcpy(c.name, name, n);

    _mesh::markSeen(_mesh::keyOf(c.originId, c.seq));   // don't relay our own echo
    _mesh::enqueueCue(c, MESH_BURST, MESH_BURST_GAP, 0);
    return true;
}

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
                             const char* detail, uint32_t uptimeSecs,
                             const char* fwHash = "unknown") {
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
    if (fwHash) strncpy(c.fwHash, fwHash, sizeof(c.fwHash) - 1);
    esp_now_send(_mesh::BCAST, (const uint8_t*)&c, sizeof(c));
}

inline void mesh_on_fw_offer(MeshFwOfferCb cb) { _mesh::S().fwOfferCb = cb; }
inline void mesh_on_ota_ready(MeshOtaReadyCb cb) { _mesh::S().otaReadyCb = cb; }

inline void mesh_send_ota_announce(const char* ssid, const char* pass, const char* env,
                                   uint16_t targetId, uint16_t windowSecs,
                                   const char* md5) {
    MeshOtaAnnounce a = {};
    a.magic0 = 'P'; a.magic1 = 'M'; a.msgType = MESH_MSG_OTA_ANNOUNCE;
    a.originType = _mesh::S().originType; a.originId = _mesh::S().originId;
    if (ssid) strncpy(a.ssid, ssid, sizeof(a.ssid) - 1);
    if (pass) strncpy(a.pass, pass, sizeof(a.pass) - 1);
    if (env) {
        size_t n = strlen(env);
        if (n >= sizeof(a.env)) memcpy(a.env, env, sizeof(a.env));
        else strncpy(a.env, env, sizeof(a.env) - 1);
    }
    a.idFilter = targetId; a.windowSecs = windowSecs;
    if (md5) strncpy(a.md5, md5, sizeof(a.md5) - 1);
    for (uint8_t i = 0; i < 3; ++i) {
        esp_now_send(_mesh::BCAST, (const uint8_t*)&a, sizeof(a));
        if (i < 2) delay(80);
    }
}

inline void mesh_send_fw_offer(uint16_t targetId, const char* env,
                               const char* buildHash, const char* sha256, uint32_t size) {
    MeshFwOffer o = {};
    o.magic0 = 'P'; o.magic1 = 'M'; o.msgType = MESH_MSG_FW_OFFER;
    o.originType = _mesh::S().originType;
    o.originId = _mesh::S().originId;
    o.targetId = targetId;
    if (env) strncpy(o.env, env, sizeof(o.env) - 1);
    if (buildHash) strncpy(o.buildHash, buildHash, sizeof(o.buildHash) - 1);
    if (sha256) strncpy(o.sha256, sha256, sizeof(o.sha256) - 1);
    o.size = size;
    for (uint8_t i = 0; i < 3; ++i) {
        esp_now_send(_mesh::BCAST, (const uint8_t*)&o, sizeof(o));
        if (i < 2) delay(80);
    }
}

inline void mesh_on_fw_status(MeshFwStatusCb cb) { _mesh::S().fwStatusCb = cb; }
inline void mesh_send_fw_status(uint8_t state, const char* env, const char* buildHash,
                                const char* sha256, int16_t error = 0) {
    MeshFwStatus s = {};
    s.magic0 = 'P'; s.magic1 = 'M'; s.msgType = MESH_MSG_FW_STATUS;
    s.originType = _mesh::S().originType; s.originId = _mesh::S().originId;
    s.state = state; s.error = error;
    if (env) strncpy(s.env, env, sizeof(s.env) - 1);
    if (buildHash) strncpy(s.buildHash, buildHash, sizeof(s.buildHash) - 1);
    if (sha256) strncpy(s.sha256, sha256, sizeof(s.sha256) - 1);
    esp_now_send(_mesh::BCAST, (const uint8_t*)&s, sizeof(s));
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
    if (st.fwOfferFull) {
        MeshFwOffer offer = st.fwOfferRx;
        st.fwOfferFull = false;
        if (st.fwOfferCb) st.fwOfferCb(offer);
    }
    while (st.fwStatusTail != st.fwStatusHead) {
        MeshFwStatus status = st.fwStatusRx[st.fwStatusTail];
        st.fwStatusTail = (st.fwStatusTail + 1) & 7;
        if (st.fwStatusCb) st.fwStatusCb(status);
    }
    while (st.otaReadyTail != st.otaReadyHead) {
        MeshOtaReady status = st.otaReadyRx[st.otaReadyTail];
        st.otaReadyTail = (st.otaReadyTail + 1) & 7;
        if (st.otaReadyCb) st.otaReadyCb(status);
    }

    while (st.cueTail != st.cueHead) {
        MeshCue c = st.cueRx[st.cueTail];
        st.cueTail = (st.cueTail + 1) & 3;
        _mesh::handleCue(c);
    }

    while (st.compTail != st.compHead) {
        MeshComponents c = st.compRx[st.compTail];
        st.compTail = (st.compTail + 1) & 3;
        _mesh::handleComponents(c);
    }

    while (st.cfgTail != st.cfgHead) {
        _mesh::handleConfig(st.cfgRx[st.cfgTail]);
        st.cfgTail = (st.cfgTail + 1) & 1;
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
    for (uint8_t i = 0; i < 4; i++) {
        _mesh::ComponentsSlot& s = st.compOut[i];
        if (!s.used || now < s.dueMs) continue;
        esp_err_t e = esp_now_send(_mesh::BCAST, (const uint8_t*)&s.pkt, sizeof(MeshComponents));
        st.statSent++;
        if (e != ESP_OK) st.statSendErr++;
        if (--s.left == 0) s.used = false;
        else s.dueMs = now + s.gapMs;
    }
    for (uint8_t i = 0; i < 2; i++) {
        _mesh::ConfigSlot& s = st.cfgOut[i];
        if (!s.used || now < s.dueMs) continue;
        esp_err_t e = esp_now_send(_mesh::BCAST, (const uint8_t*)&s.pkt, mesh_config_wire_size(s.pkt));
        st.statSent++;
        if (e != ESP_OK) st.statSendErr++;
        if (--s.left == 0) s.used = false;
        else s.dueMs = now + s.gapMs;
    }
    for (uint8_t i = 0; i < 8; i++) {
        _mesh::CueSlot& s = st.cueOut[i];
        if (!s.used || now < s.dueMs) continue;
        esp_err_t e = esp_now_send(_mesh::BCAST, (const uint8_t*)&s.cue, mesh_cue_wire_size(s.cue));
        st.statSent++;
        if (e != ESP_OK) st.statSendErr++;
        if (--s.left == 0) s.used = false;
        else s.dueMs = now + s.gapMs;
    }
}

}  // namespace pulleys
