#pragma once

#include <stdint.h>

// ── pulleys_cues — fingerprint of a game's cue table ─────────────────────────
//
// A cue crosses the mesh as [folder, file] plus this hash (see GAME_FORMAT.md).
// The two bytes mean nothing without the cue table that numbers them, so every
// cue packet also names the table it was numbered against. A receiver whose
// own table hashes differently knows the index is not safe to look up.
//
// FNV-1a over each group name (then 0x01) and each cue name (then 0x00), in
// file order, folded to 16 bits. tools/game_to_json.py computes the same thing
// -- change one and the other has to follow.
//
//   uint32_t h = pulleys::cues_hash_begin();
//   h = pulleys::cues_hash_add(h, "audio_loop", 1);
//   h = pulleys::cues_hash_add(h, "audio_loop_idle", 0);
//   uint16_t fingerprint = pulleys::cues_hash_end(h);

namespace pulleys {

inline uint32_t cues_hash_begin() { return 2166136261u; }

inline uint32_t cues_hash_add(uint32_t h, const char* s, uint8_t sep) {
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    h ^= sep;
    h *= 16777619u;
    return h;
}

inline uint16_t cues_hash_end(uint32_t h) { return (uint16_t)((h >> 16) ^ h); }

}  // namespace pulleys
