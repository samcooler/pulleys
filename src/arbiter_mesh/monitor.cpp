#include "monitor.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>

static MonNode     s_nodes[MON_MAX_NODES];
static MonChannel  s_chans[MON_CHANNELS];
static MonLogEntry s_log[MON_LOG_LEN];
static uint8_t     s_logCount = 0;

static uint32_t s_totalEvents   = 0;
static uint32_t s_relayedEvents = 0;

// Events/min over a rolling 60-slot ring of per-second counts.
static uint16_t s_perSec[60];
static uint8_t  s_secHead = 0;

static constexpr float ACT_HALFLIFE_S = 25.0f;   // matches the Screen's model

static MonNode* findOrAdd(uint16_t id, uint8_t type) {
    MonNode* freeSlot = nullptr;
    for (uint8_t i = 0; i < MON_MAX_NODES; i++) {
        if (s_nodes[i].active && s_nodes[i].id == id) return &s_nodes[i];
        if (!s_nodes[i].active && !freeSlot) freeSlot = &s_nodes[i];
    }
    if (!freeSlot) return nullptr;
    memset(freeSlot, 0, sizeof(MonNode));
    freeSlot->active      = true;
    freeSlot->id          = id;
    freeSlot->type        = type;
    freeSlot->firstSeenMs = millis();
    return freeSlot;
}

// ── Mesh callbacks ────────────────────────────────────────────────────────────
static void onEvent(const pulleys::MeshEvent& ev, bool relayed) {
    uint32_t now = millis();

    MonNode* n = findOrAdd(ev.originId, ev.originType);
    if (n) {
        n->type          = ev.originType;
        n->channel       = ev.channel;
        n->mode          = ev.mode;
        n->events++;
        n->lastEventMs   = now;
        n->lastMagnitude = ev.magnitude;
        // An event proves liveness even if we have missed the node's beacons.
        if (n->lastBeaconMs == 0) n->lastBeaconMs = now;
    }

    if (ev.channel < MON_CHANNELS) {
        MonChannel& c = s_chans[ev.channel];
        c.events++;
        c.lastEventMs = now;
        c.activity   += 1.0f;
    }

    s_totalEvents++;
    if (relayed) s_relayedEvents++;
    s_perSec[s_secHead]++;

    // Push onto the log, newest first
    for (int i = MON_LOG_LEN - 1; i > 0; i--) s_log[i] = s_log[i - 1];
    s_log[0] = { now, ev.originId, ev.channel, ev.magnitude, ev.ttl, relayed };
    if (s_logCount < MON_LOG_LEN) s_logCount++;
}

static void onSync(uint8_t originType, uint16_t originId, int32_t skewMs) {
    if (originId == 0) return;                 // pre-id firmware — nothing to track
    MonNode* n = findOrAdd(originId, originType);
    if (!n) return;
    n->type         = originType;
    n->lastBeaconMs = millis();
    n->skewMs       = skewMs;
}

// ── Public ────────────────────────────────────────────────────────────────────
void monitor_init() {
    memset(s_nodes, 0, sizeof(s_nodes));
    memset(s_chans, 0, sizeof(s_chans));
    memset(s_log,   0, sizeof(s_log));
    memset(s_perSec, 0, sizeof(s_perSec));
    pulleys::mesh_on_event(onEvent);
    pulleys::mesh_on_sync(onSync);
}

void monitor_tick() {
    uint32_t now = millis();

    // Decay channel activity on the same curve the Screens use, so the bars
    // here match what the matrices are actually showing.
    float k = powf(0.5f, 1.0f / ACT_HALFLIFE_S);
    for (uint8_t i = 0; i < MON_CHANNELS; i++) {
        s_chans[i].activity *= k;
        if (s_chans[i].activity < 0.002f) s_chans[i].activity = 0.0f;
    }

    // Count distinct sensors per channel
    uint8_t perCh[MON_CHANNELS] = {};
    for (uint8_t i = 0; i < MON_MAX_NODES; i++) {
        const MonNode& n = s_nodes[i];
        if (n.active && n.type == pulleys::MESH_ORIGIN_SENSOR && n.events > 0 &&
            n.channel < MON_CHANNELS) perCh[n.channel]++;
    }
    for (uint8_t i = 0; i < MON_CHANNELS; i++) s_chans[i].sensors = perCh[i];

    // Drop nodes that have gone quiet long enough to be considered gone
    for (uint8_t i = 0; i < MON_MAX_NODES; i++) {
        MonNode& n = s_nodes[i];
        if (!n.active) continue;
        uint32_t quiet = now - (n.lastBeaconMs > n.lastEventMs ? n.lastBeaconMs : n.lastEventMs);
        if (quiet > NODE_LOST_MS * 3) n.active = false;
    }

    s_secHead = (s_secHead + 1) % 60;
    s_perSec[s_secHead] = 0;
}

const MonNode*     monitor_nodes()     { return s_nodes; }
const MonChannel*  monitor_channels()  { return s_chans; }
const MonLogEntry* monitor_log()       { return s_log; }
uint8_t            monitor_log_count() { return s_logCount; }
uint32_t           monitor_total_events()   { return s_totalEvents; }
uint32_t           monitor_relayed_events() { return s_relayedEvents; }

uint8_t monitor_count(uint8_t type) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MON_MAX_NODES; i++)
        if (s_nodes[i].active && s_nodes[i].type == type) n++;
    return n;
}

float monitor_events_per_min() {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 60; i++) sum += s_perSec[i];
    return (float)sum;   // a full 60-second ring is already per-minute
}

int32_t monitor_skew_spread() {
    int32_t lo = 0, hi = 0;
    bool any = false;
    for (uint8_t i = 0; i < MON_MAX_NODES; i++) {
        if (!s_nodes[i].active || s_nodes[i].lastBeaconMs == 0) continue;
        int32_t s = s_nodes[i].skewMs;
        if (!any) { lo = hi = s; any = true; }
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    return any ? (hi - lo) : 0;
}
