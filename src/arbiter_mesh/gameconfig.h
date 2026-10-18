#pragma once

#include <stdint.h>
#include <Arduino.h>

// ── Game config — getting a game into the arbiter ────────────────────────────
//
// The arbiter boots into the game baked in at build time (tools/embed_game.py,
// from state_config.yaml), unless a replacement was posted earlier and is
// still in flash. tools/game_to_json.py posts one. The running game itself
// lives in gamestate.cpp; this file is only the front door.
//
//   POST   /config   body = JSON. Checked, stored, and live at once: the game
//                    restarts at its `start` state. 200 on success; 400 with a
//                    reason if it is not a valid game.
//   GET    /config   the stored JSON, or 404 if running the built-in default.
//   DELETE /config   forget the stored game and go back to the built-in one.
//   POST   /event    inject a game event, as if the sensors had caused it:
//                    body `component_activated` or `name=component_activated`.
//   GET    /state    one line of JSON: game, state, time in state, cue hash.
//
// The endpoints live on a SoftAP raised on MESH_WIFI_CHANNEL, so ESP-NOW keeps
// working underneath it (the same trick lib/pulleys_ota plays). Join
// "pulleys-arbiter" and the board is at 192.168.4.1, port 8080 -- not 80, which
// the OTA host owns while it runs.
//
// A stored game is kept in LittleFS (/game.json), so it survives a reboot and
// does not depend on the SD card being in. The game's *state* is never kept:
// every boot starts at `start`.

void gameconfig_init();     // after the mesh is up
void gameconfig_poll();     // serve pending requests; call every loop
