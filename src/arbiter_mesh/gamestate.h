#pragma once

#include <stdint.h>
#include <Arduino.h>
#include "gameevents.h"

// ── Game state machine — runs the game config ────────────────────────────────
//
// Runs the JSON form of game_state_machine/state_config.yaml (see
// GAME_FORMAT.md): events come in, the current state reacts by firing cues or
// moving to another state. It mirrors overlord/game.py, which does the same on
// the Pi.
//
// A cue it fires leaves as a MeshCue (pulleys_mesh.h), its name as ASCII, for
// the bridge to hand to the Pi. A config is adopted whole: loading one restarts the game at its
// `start` state with no timers carried over, and nothing is remembered across
// a reboot -- the machine always begins at `start`.
//
// Events arrive through gamestate_event(), the only way in; components.cpp is
// its source. Everything runs on the main loop, so there is no locking.

// Validate and adopt a config. On failure the running game is untouched and
// `why` says what is wrong.
bool gamestate_load(const String& json, String& why);

void gamestate_event(GameEvent e);
void gamestate_tick();                 // every loop: the `after` timer

bool        gamestate_ready();         // a config is loaded
const char* gamestate_game();          // its `game` field
const char* gamestate_state();         // current state name
float       gamestate_decay_rate();    // percent of full charge a lit component loses per second

// Which component a rope channel drives, from the config's `components`: 0-4 are
// the major components, 5-7 the final ones. -1 if the channel drives nothing.
int         gamestate_component_for_channel(uint8_t channel);

uint32_t    gamestate_state_age_ms();      // time in the current state
int32_t     gamestate_timer_left_ms();     // until `after` fires; -1 if none
const char* gamestate_timer_target();      // the state `after` leads to

// What happened, newest first, for the panel.
//   E  an event the state reacted to    C  a cue fired
//   S  moved to a new state             T  the `after` timer ran out
struct GameTrigger {
    uint32_t atMs;
    char     kind;
    char     text[44];
};
#define GAME_LOG_LEN 8
const GameTrigger* gamestate_log();
uint8_t            gamestate_log_count();
uint32_t           gamestate_trigger_seq();   // bumps on every entry: "something just fired"
