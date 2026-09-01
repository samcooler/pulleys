#pragma once

#include <stdint.h>

// ── pulleys_install — the per-board install map ───────────────────────────────
//
// Facts about the physical piece that no board can work out for itself: which
// rope a Sensor is tied to, and which display a Screen is meant to be running.
// Keyed by the device ID identity_init() derives from the MAC, which is the
// same ID a board prints in its whoami line, so a board is addressable before
// it has been configured and stays addressable after it is reflashed.
//
// This lives in the repo rather than in each board's NVS on purpose: a swapped
// board should need flashing and nothing else, and the answer to "what was that
// one set to?" should be in version control rather than in someone's memory.
//
// Two ways in, both intended, for two different moments:
//
//   ./flash_all.sh with no arguments — the install. Adds every attached board
//   that is missing, builds, and reflashes with the result. It does not know
//   which rope is which or which display belongs where; it exists so a crate of
//   boards works at all, unattended, without anyone deciding anything.
//
//   Editing these blocks by hand — the calm moment. Which board drives which
//   rope, and which screen shows which display, are judgements about the
//   physical piece that nothing can infer from a MAC address. Setting them
//   deliberately is the better answer, not a deviation from the tool's.
//   Reflash afterwards.
//
// The two do not fight: tools/install_map.py only ever adds IDs it does not
// already find here, so anything set by hand survives every later run. Keep the
// marker comments, and keep values in range.

namespace pulleys {

// ── Sensors: which rope, and how it is read ───────────────────────────────────
// Detection mode. These mirror SENSOR_MODE_ROTATION / _LINEAR in pulleys_mesh.h
// rather than including it — this header is meant to stay free of the radio —
// and src/sensor/main.cpp static_asserts that they still agree.
//
// ROT counts turns of the pulley; LIN watches for a pull along a learned axis.
// Which one a rope wants is a fact about how it is rigged, so it belongs here
// beside the channel rather than in the board's NVS. LIN is the default, for
// new rows and for a board nobody has listed: most ropes are pulled rather
// than spun, so it is the answer that is right more often when nobody has said.
static constexpr uint8_t ROT = 0;
static constexpr uint8_t LIN = 1;

inline const char* sensor_mode_name(uint8_t m) { return m == LIN ? "lin" : "rot"; }


// Several boards may share a channel, and that is a real configuration rather
// than a mistake: ropes on one channel report as one rope, which is what you
// want of a cluster meant to read as a single thing. New boards are given the
// least-used channel, so sharing happens when you ask for it or when there are
// more ropes than channels — not by accident.
struct ChannelAssignment { uint16_t id; uint8_t channel; uint8_t mode; };

static const ChannelAssignment CHANNEL_ASSIGNMENT[] = {
    //   id     ch  mode
    // ASSIGNMENTS BEGIN
    { 0xEC52, 1, LIN },   // N-EC52
    { 0x0664, 2, LIN },   // N-0664
    // ASSIGNMENTS END
};
static constexpr uint8_t CHANNEL_ASSIGNMENT_COUNT =
    sizeof(CHANNEL_ASSIGNMENT) / sizeof(CHANNEL_ASSIGNMENT[0]);

// Channels an unlisted board may fall back to. Channel 0 is deliberately
// excluded: it is the value a board lands on when something has gone wrong
// (an empty NVS read, a zeroed struct), so keeping it out of normal use means
// a rope reporting on channel 0 is always a fault and never a coincidence.
static constexpr uint8_t CHANNEL_FALLBACK_LO = 1;
static constexpr uint8_t CHANNEL_FALLBACK_HI = 12;

// Fallback for a board the table does not name: derive a channel from its ID.
// Two unlisted boards can still collide — with 12 channels a handful of boards
// collide sooner than intuition suggests — so this is a way to keep an
// unregistered board useful and visibly distinct, not a substitute for the
// table. The ID is already a MAC hash; mixing again decorrelates the low bits
// so that boards from one production batch do not land together.
inline uint8_t channel_from_id(uint16_t id) {
    uint32_t h = (uint32_t)id * 2654435761U;
    h ^= h >> 16;
    uint8_t span = CHANNEL_FALLBACK_HI - CHANNEL_FALLBACK_LO + 1;
    return (uint8_t)(CHANNEL_FALLBACK_LO + (h % span));
}

// The row for this board, or nullptr if it is not in the table.
inline const ChannelAssignment* sensor_row(uint16_t id) {
    for (uint8_t i = 0; i < CHANNEL_ASSIGNMENT_COUNT; i++)
        if (CHANNEL_ASSIGNMENT[i].id == id) return &CHANNEL_ASSIGNMENT[i];
    return nullptr;
}

// The channel this board is assigned, or -1 if it is not in the table.
inline int8_t channel_for_device(uint16_t id) {
    const ChannelAssignment* r = sensor_row(id);
    return r ? (int8_t)r->channel : (int8_t)-1;
}

// The detection mode this board is assigned, or -1 if it is not in the table.
inline int8_t mode_for_device(uint16_t id) {
    const ChannelAssignment* r = sensor_row(id);
    return r ? (int8_t)r->mode : (int8_t)-1;
}

// ── Screens: which display ────────────────────────────────────────────────────
// The names here are the ones install_map.py accepts and flash_all prints, so
// changing one changes the tool's vocabulary too.
enum ScreenDisplay : uint8_t {
    SCREEN_COUNTER = 0,   // per-channel detection counts, side by side
    SCREEN_RANKING = 1,   // top-4 channels as 8×8 shape patterns
    SCREEN_DISPLAY_COUNT
};

inline const char* screen_display_name(uint8_t d) {
    switch (d) {
        case SCREEN_COUNTER: return "counter";
        case SCREEN_RANKING: return "ranking";
        default:             return "?";
    }
}

struct DisplayAssignment { uint16_t id; uint8_t display; };

static const DisplayAssignment DISPLAY_ASSIGNMENT[] = {
    // DISPLAYS BEGIN
    // DISPLAYS END
};
static constexpr uint8_t DISPLAY_ASSIGNMENT_COUNT =
    sizeof(DISPLAY_ASSIGNMENT) / sizeof(DISPLAY_ASSIGNMENT[0]);

// The display this Screen is assigned, or -1 if it is not in the table. An
// unlisted Screen keeps the old behaviour — stepping to the next display on
// every power cycle — which is still the only interface a matrix with no
// buttons has when nobody has decided for it.
inline int8_t display_for_device(uint16_t id) {
    for (uint8_t i = 0; i < DISPLAY_ASSIGNMENT_COUNT; i++)
        if (DISPLAY_ASSIGNMENT[i].id == id)
            return (int8_t)DISPLAY_ASSIGNMENT[i].display;
    return -1;
}

}  // namespace pulleys
