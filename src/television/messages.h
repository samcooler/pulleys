#pragma once
#include <stdint.h>

// ── What the television shows ─────────────────────────────────────────────────
// Every message belongs to a channel. The game picks the channel with the
// device setting `television.channel` (game_state_machine/state_config.yaml),
// and the set plays that channel's messages in table order, one after another,
// starting over at the end. A channel with one message just holds it.
//
//   channel  which channel it is on: a lowercase word
//   text     wrapped and centred, as large as it will fit. "\n" forces a break.
//   bg, fg   0xRRGGBB: the background, and the text on it
//   seconds  how long it holds before the set changes to the next
//
// The channel names here and the values listed for television.channel under
// devices: in the game file must agree -- test/test_television.py checks it.
// `dark` is not a channel: it is the screen off, backlight and all.
//
// TV_DEFAULT_CHANNEL is what plays when the game has not said: at boot before
// the arbiter is heard, and when it has gone quiet past the settings lease.
//
// The words here are placeholders until the real ones are written.
#define TV_DEFAULT_CHANNEL "idle"

#include <pulleys_textcard.h>

static const pulleys::TextCard TV_MESSAGES[] = {
    { "idle",     "PLACEHOLDER\nidle one",                         0x10206A, 0xFFFFFF, 6 },
    { "idle",     "STAY TUNED",                                    0xC81E1E, 0xFFF2C0, 5 },
    { "janis",    "A message from Janis",                          0x000000, 0xFFD400, 6 },
    { "janis",    "PLACEHOLDER\nJanis two",                        0x3A0010, 0xFFFFFF, 6 },
    { "paradise", "The spirits are resting.\nPlease do not disturb them.", 0x0A3D2E, 0xE8FFF4, 8 },
};

static constexpr uint8_t TV_MESSAGE_COUNT = sizeof(TV_MESSAGES) / sizeof(TV_MESSAGES[0]);
static_assert(TV_MESSAGE_COUNT > 0, "the television needs at least one message");
