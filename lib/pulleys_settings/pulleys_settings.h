#pragma once

#include <Arduino.h>
#include <pulleys_mesh.h>

// ── pulleys_settings — the game's settings for every device, kept in sync ────
//
// The game (the arbiter's state machine) says what each device should be doing
// -- `set: { diorama.lights: creepy }` in the game file -- and that value holds
// until the game sets another. It is a table, not a message: the arbiter
// broadcasts the whole table whenever it changes and again every second, so a
// device that missed a packet, or rebooted, is right again within a second.
// Cues stay what they were for: one-shot media, played once.
//
// Every value is held on a lease. A device that has not heard the table for
// SETTINGS_LEASE_MS treats every setting as unset and falls back to whatever
// its own firmware calls safe -- lights out, door locked. The arbiter going
// quiet must never leave the room stuck in its last state.
//
// The other direction is MESH_MSG_DEVICE. A device emits its inputs (a door
// opening) as one-shot events, which the game sees by name like any other
// event (`on: diorama.door_opened:`), and reports what it is actually doing
// when that changes and every SETTINGS_REPORT_MS, so the arbiter can show
// "latch: open" as fact rather than intent. Reports are deliberately slow:
// every node relays every packet, so a once-a-second report from each device
// would cost the mesh the square of its size.
//
// On the air a setting is two 16-bit hashes: of its name ("diorama.lights")
// and of its value ("creepy"). The names stay readable in the game file and in
// firmware, and tools/game_to_json.py refuses a game where two of them collide.
// 0 is never a hash: it means "unset".
//
//   Device:
//     pulleys::settings_init();                               // after mesh_init
//     if (pulleys::setting_is("diorama.lights", "creepy")) ...
//     pulleys::device_emit("diorama.door_opened");
//     pulleys::device_report("diorama.lights", "creepy");     // what it is doing
//     pulleys::settings_poll();                               // every loop
//
//   A device reads its settings every loop rather than waiting to be told:
//   that one path covers a change, a reboot and the lease running out.
//
//   Arbiter: settings_send() the table; mesh_on_flood(MESH_MSG_DEVICE, ...)
//   and settings_decode_device() for what comes back.

namespace pulleys {

static constexpr uint32_t SETTINGS_LEASE_MS  = 12000;  // silence before a device falls back to safe
static constexpr uint32_t SETTINGS_REPORT_MS = 20000;  // a device's unprompted report, at most this often
static constexpr uint8_t  SETTINGS_MAX       = 48;     // entries in one table (fits a frame: 5 + 4*48)
static constexpr uint8_t  SETTINGS_REPORT_MAX = 8;     // settings one device reports

// FNV-1a over the name, folded to 16 bits; never 0. constexpr, so a device's
// names cost nothing at run time. Keep in step with name_hash() in
// tools/game_to_json.py.
constexpr uint32_t _fnv(const char* s, uint32_t h) {
    return *s ? _fnv(s + 1, (h ^ (uint8_t)*s) * 16777619u) : h;
}
constexpr uint16_t _fold(uint32_t h) {
    return ((h >> 16) ^ (h & 0xFFFF)) ? (uint16_t)((h >> 16) ^ (h & 0xFFFF)) : (uint16_t)1;
}
constexpr uint16_t settings_hash(const char* s) { return _fold(_fnv(s, 2166136261u)); }

struct __attribute__((packed)) SettingEntry {
    uint16_t key;     // settings_hash("device.setting")
    uint16_t value;   // settings_hash("value"), 0 = unset
};

// MESH_MSG_STATE body: the arbiter's whole table.
struct __attribute__((packed)) SettingsState {
    uint16_t     epoch;     // random per arbiter boot: a new arbiter is believed at once
    uint16_t     version;   // bumps on every change within an epoch
    uint8_t      count;
    SettingEntry entry[SETTINGS_MAX];
};
static constexpr uint8_t SETTINGS_STATE_HEADER = 5;
static_assert(SETTINGS_STATE_HEADER + SETTINGS_MAX * sizeof(SettingEntry) <= MESH_FLOOD_MAX_BODY,
              "the table has to fit one frame");

// MESH_MSG_DEVICE body.
enum : uint8_t {
    DEVICE_INPUT  = 1,   // entry[0].key = settings_hash("device.input"), value 0
    DEVICE_REPORT = 2,   // entries = what this device is doing, as settings
};
struct __attribute__((packed)) DeviceMsg {
    uint8_t      kind;
    uint8_t      count;
    SettingEntry entry[SETTINGS_REPORT_MAX];
};
static constexpr uint8_t DEVICE_MSG_HEADER = 2;

// ── Device side ──────────────────────────────────────────────────────────────
namespace _settings {
struct State {
    SettingEntry table[SETTINGS_MAX] = {};
    uint8_t      count     = 0;
    uint16_t     epoch     = 0;
    uint16_t     version   = 0;
    bool         heard     = false;
    uint32_t     heardAt   = 0;
    SettingEntry report[SETTINGS_REPORT_MAX] = {};
    uint8_t      reportCount = 0;
    bool         reportDirty = false;
    uint32_t     reportedAt  = 0;
};
inline State& S() { static State s; return s; }

inline void onState(const MeshFlood& f, bool /*relayed*/) {
    if (f.len < SETTINGS_STATE_HEADER) return;
    SettingsState in = {};
    memcpy(&in, f.body, f.len < sizeof(in) ? f.len : sizeof(in));
    if (in.count > SETTINGS_MAX || f.len < SETTINGS_STATE_HEADER + in.count * sizeof(SettingEntry)) return;
    State& s = S();
    // A copy relayed late can arrive after a newer one; keep the newer. A new
    // epoch is a rebooted arbiter, whose count started again.
    if (s.heard && in.epoch == s.epoch && (int16_t)(in.version - s.version) < 0) return;
    memcpy(s.table, in.entry, in.count * sizeof(SettingEntry));
    s.count   = in.count;
    s.epoch   = in.epoch;
    s.version = in.version;
    s.heardAt = millis();
    s.heard   = true;
}
}  // namespace _settings

inline void settings_init() { mesh_on_flood(MESH_MSG_STATE, _settings::onState); }

// The table is current: heard within the lease.
inline bool settings_fresh() {
    const _settings::State& s = _settings::S();
    return s.heard && millis() - s.heardAt < SETTINGS_LEASE_MS;
}

// The value's hash, or 0 if unset -- never set, or the lease has run out.
inline uint16_t setting_value(uint16_t key) {
    if (!settings_fresh()) return 0;
    const _settings::State& s = _settings::S();
    for (uint8_t i = 0; i < s.count; i++) if (s.table[i].key == key) return s.table[i].value;
    return 0;
}
inline bool setting_is(const char* key, const char* value) {
    return setting_value(settings_hash(key)) == settings_hash(value);
}

// Bench: behave as if the arbiter had just sent this setting. It holds for one
// lease, like anything heard, and the next real table replaces it.
inline void settings_pretend(const char* key, const char* value) {
    _settings::State& s = _settings::S();
    uint16_t k = settings_hash(key), v = settings_hash(value);
    uint8_t i = 0;
    while (i < s.count && s.table[i].key != k) i++;
    if (i == s.count) {
        if (s.count >= SETTINGS_MAX) return;
        s.count++;
    }
    s.table[i] = { k, v };
    s.heard   = true;
    s.heardAt = millis();
}

// A one-shot input, for the game to react to. Sent as a burst, like a pull.
inline void device_emit(const char* input) {
    DeviceMsg m = {};
    m.kind  = DEVICE_INPUT;
    m.count = 1;
    m.entry[0].key = settings_hash(input);
    mesh_send_flood(MESH_MSG_DEVICE, &m, DEVICE_MSG_HEADER + sizeof(SettingEntry));
}

// What this device is actually doing. Sent when it changes (as a short burst),
// otherwise every SETTINGS_REPORT_MS by settings_poll().
inline void device_report(const char* key, const char* value) {
    _settings::State& s = _settings::S();
    uint16_t k = settings_hash(key), v = settings_hash(value);
    for (uint8_t i = 0; i < s.reportCount; i++) {
        if (s.report[i].key != k) continue;
        if (s.report[i].value != v) { s.report[i].value = v; s.reportDirty = true; }
        return;
    }
    if (s.reportCount >= SETTINGS_REPORT_MAX) return;
    s.report[s.reportCount++] = { k, v };
    s.reportDirty = true;
}

inline void settings_poll() {
    _settings::State& s = _settings::S();
    if (!s.reportCount) return;
    uint32_t now = millis();
    if (!s.reportDirty && now - s.reportedAt < SETTINGS_REPORT_MS) return;
    DeviceMsg m = {};
    m.kind  = DEVICE_REPORT;
    m.count = s.reportCount;
    memcpy(m.entry, s.report, s.reportCount * sizeof(SettingEntry));
    mesh_send_flood(MESH_MSG_DEVICE, &m, DEVICE_MSG_HEADER + s.reportCount * sizeof(SettingEntry),
                    s.reportDirty ? 3 : 1, 120);
    s.reportDirty = false;
    s.reportedAt  = now;
}

// ── Arbiter side ─────────────────────────────────────────────────────────────
// The whole table. `urgent` is a change, sent as a burst; otherwise the repeat.
inline void settings_send(uint16_t epoch, uint16_t version, const SettingEntry* entries,
                          uint8_t count, bool urgent) {
    if (count > SETTINGS_MAX) count = SETTINGS_MAX;
    SettingsState st = {};
    st.epoch   = epoch;
    st.version = version;
    st.count   = count;
    memcpy(st.entry, entries, count * sizeof(SettingEntry));
    mesh_send_flood(MESH_MSG_STATE, &st, SETTINGS_STATE_HEADER + count * sizeof(SettingEntry),
                    urgent ? 3 : 1, 80);
}

// A MESH_MSG_DEVICE body, checked. False if it is malformed.
inline bool settings_decode_device(const MeshFlood& f, DeviceMsg& out) {
    if (f.len < DEVICE_MSG_HEADER) return false;
    out = {};
    memcpy(&out, f.body, f.len < sizeof(out) ? f.len : sizeof(out));
    return out.count <= SETTINGS_REPORT_MAX &&
           f.len >= DEVICE_MSG_HEADER + out.count * sizeof(SettingEntry);
}

}  // namespace pulleys
