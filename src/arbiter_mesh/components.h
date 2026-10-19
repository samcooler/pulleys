#pragma once

#include <pulleys_mesh.h>

// ── Components — what the ropes are doing, as game events ────────────────────
//
// The low-level side of the seam. It sees raw sensor events (channel,
// magnitude) and decides when a *component* has been activated or deactivated,
// handing each decision to gamestate_event(). The state machine never sees a
// channel; this file never sees a state.
//
// The rules are a placeholder, mirroring overlord/simulate.py so the two can
// be compared: each component has a rope channel (the config's `components`
// section, in state_config.yaml). A pull lights its component, and it stays
// lit while it drains from 100% at the game's default_decay_rate (see
// state_config.yaml), and a further pull tops it back up. Replace the body of
// this module when the real rules are settled; nothing outside it needs to
// change.
//
// It also publishes every component's charge for the displays (MeshComponents):
// on each change, and once a second otherwise.

void components_init();
void components_on_mesh_event(const pulleys::MeshEvent& ev);   // every deduped sensor event
void components_tick();                                        // every loop: wear-out
