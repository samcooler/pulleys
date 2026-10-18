#pragma once

void ui_init();
void ui_refresh();   // ~1 Hz: the monitor panels and the game strip's clocks
void ui_tick();      // every loop: the game strip's trigger flash
