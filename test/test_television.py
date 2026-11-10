"""The television's channels and the game file have to agree (no hardware required).

The game picks a channel with the device setting television.channel; the
firmware only knows the channels its messages are on. A value the game lists
that the firmware lacks plays the default channel instead, and a channel the
firmware has that the game does not list can never be chosen. Either is a
quiet mistake, so it is caught here.
"""
from pathlib import Path
import re
import unittest

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

ROOT = Path(__file__).resolve().parents[1]
MESSAGES = ROOT / "src/television/messages.h"
GAME = ROOT / "game_state_machine/state_config.yaml"
DARK = "dark"   # TV_OFF in src/television/main.cpp: the screen off, not a channel


def firmware_channels():
    text = MESSAGES.read_text()
    table = text[text.index("TV_MESSAGES[]"):]
    channels = re.findall(r'^\s*\{\s*"([^"]+)"\s*,', table, re.M)
    default = re.search(r'#define\s+TV_DEFAULT_CHANNEL\s+"([^"]+)"', text).group(1)
    return channels, default


class TelevisionChannelTests(unittest.TestCase):
    def test_messages_parse(self):
        channels, default = firmware_channels()
        self.assertTrue(channels, "no messages found in messages.h")
        for name in channels:
            self.assertRegex(name, r"^[a-z0-9_]+$", "a channel is a lowercase word")
        self.assertNotIn(DARK, channels, f"'{DARK}' is the screen off, not a channel")
        self.assertIn(default, channels, "TV_DEFAULT_CHANNEL has no messages")

    @unittest.skipIf(yaml is None, "needs pyyaml")
    def test_game_lists_exactly_the_firmware_channels(self):
        channels, _ = firmware_channels()
        game = yaml.safe_load(GAME.read_text())
        values = game["devices"]["television"]["set"]["channel"]
        self.assertEqual(set(values), set(channels) | {DARK})


if __name__ == "__main__":
    unittest.main()
