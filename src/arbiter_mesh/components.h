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
// be compared: each component has a rope channel (the table in components.cpp;
// major component 1 is channel 10). A pull lights its component, and it stays
// lit until WEAR_MS (15 s) passes with no further pull. Replace the body of
// this module when the real rules are settled; nothing outside it needs to
// change.
//
// It also publishes every component's charge for the displays (MeshComponents):
// on each change, and once a second otherwise.

void components_init();
void components_on_mesh_event(const pulleys::MeshEvent& ev);   // every deduped sensor event
void components_tick();                                        // every loop: wear-out
