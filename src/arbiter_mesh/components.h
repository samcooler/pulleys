#pragma once

#include <pulleys_mesh.h>

// ── Components — what the ropes are doing, as game events ────────────────────
//
// The low-level side of the seam. It sees raw sensor events (channel,
// magnitude) and decides when a *component* has been activated or deactivated,
// handing each decision to gamestate_event(). The state machine never sees a
// channel; this file never sees a state.
//
// Each major component is a game1 colour, driven by every rope wearing it today
// (the config's `components` section, in state_config.yaml); every purple
// channel is a final, and game2 is won when all the roped ones are lit. Each
// pull adds the game's charge_per_pull and the charge drains at its
// default_decay_rate, so only keeping at it fills one: it lights at 100% and
// stays lit until it has drained back to 0. Major activation events
// also track the Simon goal set selected by game_randomize_simon_N_goals cues
// or the `goals:` action.
//
// Every pull is also an event of its own, before its charge goes in: `pull`,
// then `goal_pull`, `nongoal_pull` or `final_pull`. A state can react to a
// pull itself (bop-it, a score) and not only to a component filling.
//
// It also publishes every component's charge for the displays (MeshComponents):
// on each change, and once a second otherwise.

void components_init();
// Called for each game cue; Simon randomization cues select the active goal set.
bool components_on_cue(const char* cue);
// The `goals: N` action: deal N goal colours (1-5), never the set just dealt.
void components_deal_goals(uint8_t count);
uint8_t components_goal_mask();   // bit i = major component i is a goal
// Send every component's charge (and, while the state cares, the goals) now.
void components_publish();
void components_check_simon_complete();
void components_on_mesh_event(const pulleys::MeshEvent& ev);   // every deduped sensor event
void components_tick();                                        // every loop: wear-out
