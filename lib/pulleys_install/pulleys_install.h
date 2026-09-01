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

// ── Sensors: which rope ───────────────────────────────────────────────────────
// Give two boards the same channel only if you mean those ropes to read as one.
struct ChannelAssignment { uint16_t id; uint8_t channel; };

static const ChannelAssignment CHANNEL_ASSIGNMENT[] = {
    // ASSIGNMENTS BEGIN
    { 0xEC52,  1 },   // N-EC52
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

// The channel this board is assigned, or -1 if it is not in the table.
inline int8_t channel_for_device(uint16_t id) {
    for (uint8_t i = 0; i < CHANNEL_ASSIGNMENT_COUNT; i++)
        if (CHANNEL_ASSIGNMENT[i].id == id)
            return (int8_t)CHANNEL_ASSIGNMENT[i].channel;
    return -1;
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
