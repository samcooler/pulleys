"""Ambient sounds: effects overlord repeats on a schedule, whatever the game is doing.

Read from the `ambient_sounds:` list in the game YAML (GAME_FORMAT.md). Each
entry has its own timer:

    - sound: audio_once_steam
      interval: 5       # seconds between plays
      variance: 1       # plus or minus, picked fresh each time

Only overlord reads this section; the arbiter never sees it.
"""

import random
import threading

try:
    import yaml
except ImportError:
    yaml = None


def load(path):
    """The valid entries of `ambient_sounds` in the YAML at `path`, as dicts with
    sound, interval and variance. A bad entry is reported and skipped, so a typo
    costs one sound and not the whole panel."""
    if yaml is None:
        print("ambient: pyyaml is not installed; no ambient sounds", flush=True)
        return []
    try:
        with open(path) as f:
            config = yaml.safe_load(f) or {}
    except OSError as e:
        print(f"ambient: could not read {path}: {e}", flush=True)
        return []
    entries = []
    for i, e in enumerate(config.get("ambient_sounds") or []):
        try:
            sound, interval, variance = e["sound"], float(e["interval"]), float(e.get("variance", 0))
            if not isinstance(sound, str) or interval <= 0 or variance < 0 or variance >= interval:
                raise ValueError("need a sound, interval > 0, and 0 <= variance < interval")
        except (KeyError, TypeError, ValueError) as err:
            reason = f"missing {err}" if isinstance(err, KeyError) else err
            print(f"ambient: skipping entry {i}: {reason}", flush=True)
            continue
        entries.append({"sound": sound, "interval": interval, "variance": variance})
    return entries


class Ambient:
    def __init__(self, entries, play):
        """play(sound_name) -> True if it played. It is not logged here: a sound
        every few seconds would drown the journal, so only failures are, once."""
        self.entries = entries
        self.play = play
        self._stop = threading.Event()
        self._threads = []

    def start(self):
        for e in self.entries:
            print(f"ambient: {e['sound']} every {e['interval']:g}s +/- {e['variance']:g}s",
                  flush=True)
            t = threading.Thread(target=self._run, args=(e,), daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self):
        self._stop.set()

    def _run(self, e):
        failing = False
        # Event.wait doubles as the sleep, so stop() ends the wait at once.
        while not self._stop.wait(e["interval"] + random.uniform(-e["variance"], e["variance"])):
            try:
                ok = self.play(e["sound"])
            except Exception as err:  # the schedule must outlive a failed play
                print(f"ambient: {e['sound']} failed: {err}", flush=True)
                ok = False
            if not ok and not failing:
                print(f"ambient: {e['sound']} did not play (will keep trying quietly)", flush=True)
            failing = not ok
