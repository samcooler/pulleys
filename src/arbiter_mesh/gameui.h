#pragma once

// ── STATES — jump the game to any state from the panel ───────────────────────
//
// A STATES button in the GAME strip's title row opens a full-screen grid with a
// button per state of the loaded game, the current one highlighted. Tapping a
// state jumps the game straight there (gamestate_goto: its entry cues, settings
// and timers run, as if a transition had led there) and closes the grid.
// RESTART goes back to the game's start (gamestate_reset).

void gameui_init();     // after ui_init, so the opener sits on top of the strip
void gameui_tick();     // every loop
bool gameui_is_open();
