#pragma once

#include <stdint.h>
#include <pulleys_mesh.h>

// ── Install config — which rope is on which channel, which colour each wears ──
//
// The arbiter is the authority for both, from the moment the piece is rigged:
// the sensor -> channel map is made on site from the channel mapping screen as
// the ropes go up, and the channel -> colour palette is changed whenever the
// day calls for it. Both are kept in NVS here and broadcast whole as
// MESH_MSG_CONFIG — a burst on every change, a repeat every few seconds — and
// every sensor and screen stores what it last heard.
//
// The first boot with nothing stored seeds the map from the repo's
// CHANNEL_ASSIGNMENT and the palette from its defaults, so an existing install
// carries over. `MAP` on serial prints the live map as rows for that table, so
// what was decided on site can be committed.
//
// The palette itself lives in pulleys_channel (the arbiter's own display reads
// it through channel_color like everyone else); this file decides when it is
// saved and sent.

void installcfg_init();     // after mesh_init
void installcfg_tick();     // every loop: sends a pending change, or the repeat

// The channel the map gives a sensor, or -1 if it is not mapped.
int  installcfg_channel_of(uint16_t id);

// Changes are batched: several in one loop go out as one burst.
bool installcfg_set_channel(uint16_t id, uint8_t channel);   // false if the map is full
bool installcfg_unmap(uint16_t id);
bool installcfg_set_color(uint8_t channel, uint8_t color);
// Permute game1's colours among themselves, leaving purple (game2) where it is,
// so ropes that shared a colour still share one and the game's split holds.
void installcfg_shuffle_game1();

uint8_t                       installcfg_row_count();
const pulleys::MeshConfigRow* installcfg_rows();

//   MAP                  print the map as CHANNEL_ASSIGNMENT rows
//   MAP <hexid> <ch>     map a sensor     MAP <hexid> -   forget one
//   COLOR                print the palette
//   COLOR <ch> <colour>  by name (red, blue, ...) or index
//   SHUFFLE              shuffle game1's colours
bool installcfg_handle_serial(const char* line);
