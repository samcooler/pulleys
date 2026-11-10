#pragma once
#include <pulleys_textcard.h>

// ── What the queue sign says ──────────────────────────────────────────────────
// The arbiter's screen, at the front of the installation, is a sign for the
// queue until someone unlocks the monitor behind it (signui.h). The game picks
// what it says with the device setting `queue.sign`
// (game_state_machine/state_config.yaml): each value is a channel here, and the
// sign plays that channel's cards in order, round again. A channel of one card
// just holds it.
//
//   channel  the queue.sign value it shows for: a lowercase word
//   text     wrapped and centred, as large as it will fit. "\n" forces a break.
//   bg, fg   0xRRGGBB: the background, and the text on it
//   seconds  how long it holds before the next card on its channel
//
// The channel names here and the values listed for queue.sign in the game file
// must agree -- test/test_television.py checks it. A value with no cards here
// leaves the sign black.
//
// The words here are first drafts.
static const pulleys::TextCard SIGN_MESSAGES[] = {
    { "enter",  "Go right in",                         0x0B6B2E, 0xFFFFFF, 10 },
    { "wait",   "Please wait to enter",                0xB3261E, 0xFFFFFF,  8 },
    { "wait",   "The next group\nwill be in shortly",  0xB3261E, 0xFFF2C0,  6 },
    { "closed", "Closed",                              0x101010, 0xC0C0C0, 10 },
};

static constexpr uint8_t SIGN_MESSAGE_COUNT = sizeof(SIGN_MESSAGES) / sizeof(SIGN_MESSAGES[0]);
static_assert(SIGN_MESSAGE_COUNT > 0, "the queue sign needs at least one card");
