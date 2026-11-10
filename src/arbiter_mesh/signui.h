#pragma once

#include <stdint.h>
#include <LovyanGFX.hpp>
#include <pulleys_gt911.h>

// ── Queue sign — the arbiter's screen as a sign for the line outside ─────────
//
// The game says what the sign shows with the device setting `queue.sign`
// (sign_messages.h has the words for each value). While the sign is up it has
// the whole screen: LVGL is paused and the cards are drawn straight to the
// panel, so the monitor and its buttons are out of reach of the public.
//
// To get to the monitor, tap the four quadrants of the screen in the secret
// order (SIGN_UNLOCK in signui.cpp), each tap within a few seconds of the
// last. Nothing on screen acknowledges a tap. The monitor has a SIGN button,
// top right, to put the sign back, and the sign comes back by itself after
// a while with no touch and no install screen open.
//
// A game with no queue.sign setting has no sign: the monitor stays up and the
// SIGN button is hidden.

void signui_init(lgfx::LovyanGFX* display, pulleys::GT911* touch);   // after ui_init
// Every loop. True while the sign has the screen: the caller must then leave
// LVGL alone (no lv_timer_handler), or it would draw over the sign.
bool signui_tick(uint32_t now, bool modalOpen);
void signui_show(bool sign);       // the sign (true) or the monitor (false)
bool signui_showing();
void signui_note_touch();          // LVGL saw a touch: the monitor is in use
