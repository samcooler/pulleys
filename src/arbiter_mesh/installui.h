#pragma once

#include <pulleys_mesh.h>

// ── Install screens — mapping ropes to channels, and channels to colours ──────
//
// Two full-screen modals over the monitor, each opened from a button in the
// title row of the quadrant it edits:
//
//   MAP ROPES (SENSORS)  pull a rope — or several, within 5 s, to put them on
//                        one channel — and they are listed; tap a channel and
//                        they move to it. Each rope flashes its new colour and
//                        reports back, and its line turns "ok". Pull the next
//                        rope and it starts over, until DONE.
//   COLOURS (CHANNELS)   tap a channel to step it through the six colours;
//                        SHUFFLE permutes game1's colours for the day.
//
// While MAP ROPES is open the ropes belong to it: their pulls do not reach the
// game, so mapping a rigged piece fires no cues.

void installui_init();      // after ui_init, so the openers sit on top
void installui_tick();      // every loop
bool installui_is_open();

// Every deduped sensor event, pulls and remap acks alike. True if the mapping
// screen took it, in which case the game must not see it.
bool installui_on_event(const pulleys::MeshEvent& ev);
