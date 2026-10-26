#pragma once

#include <pulleys_mesh.h>

// ── Components — what the ropes are doing, as game events ────────────────────
//
// The low-level side of the seam. It sees raw sensor events (channel,
// magnitude) and decides when a *component* has been activated or deactivated,
// handing each decision to gamestate_event(). The state machine never sees a
// channel; this file never sees a state.
//
// Each component has a rope channel (the config's `components` section, in
// state_config.yaml). A pull lights it, and it drains from 100% at the game's
// default_decay_rate; another pull tops it back up. Major activation events
// also track the Simon goal set selected by game_randomize_simon_N_goals cues.
//
// It also publishes every component's charge for the displays (MeshComponents):
// on each change, and once a second otherwise.

void components_init();
// Called for each game cue; Simon randomization cues select the active goal set.
bool components_on_cue(const char* cue);
void components_check_simon_complete();
void components_on_mesh_event(const pulleys::MeshEvent& ev);   // every deduped sensor event
void components_tick();                                        // every loop: wear-out
