#pragma once

#include <stdint.h>
#include <pulleys_mesh.h>

// ── Mesh monitor — the arbiter's model of the whole installation ──────────────
//
// Built entirely from what the radio hears. Sensors are known from their EVENT
// packets, Screens (which emit no events) from their clock beacons, so every
// node shows up either way.

#define MON_MAX_NODES     40
#define MON_CHANNELS      16
#define MON_LOG_LEN       14
#define NODE_STALE_MS     8000     // no beacon this long → amber
#define NODE_LOST_MS      20000    // → red, then dropped from the table

struct MonNode {
    bool     active;
    uint8_t  type;         // MESH_ORIGIN_SENSOR / _SCREEN
    uint16_t id;
    uint32_t firstSeenMs;
    uint32_t lastBeaconMs; // presence, from clock beacons
    int32_t  skewMs;       // that node's clock minus ours
    // Sensor-only
    uint8_t  channel;
    uint8_t  mode;
    uint32_t events;
    uint32_t lastEventMs;
    uint8_t  lastMagnitude;
};

struct MonChannel {
    uint32_t events;
    uint32_t lastEventMs;
    float    activity;     // decaying, drives the bar
    uint8_t  sensors;      // distinct sensors seen on this channel
};

struct MonLogEntry {
    uint32_t atMs;
    uint16_t originId;
    uint8_t  channel;
    uint8_t  magnitude;
    uint8_t  ttl;
    bool     relayed;
};

void monitor_init();
void monitor_tick();                 // ~1 Hz: decay, age out lost nodes

const MonNode*     monitor_nodes();
const MonChannel*  monitor_channels();
const MonLogEntry* monitor_log();     // newest first
uint8_t            monitor_log_count();

uint8_t  monitor_count(uint8_t type);      // active nodes of a role
uint32_t monitor_total_events();
float    monitor_events_per_min();
uint32_t monitor_relayed_events();
int32_t  monitor_skew_spread();            // worst skew across the field, ms
