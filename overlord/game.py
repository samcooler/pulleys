"""Game engine on the wall clock: runs a game_state_machine/*.yaml on the Pi.

The game itself is playtest/engine.py -- the same engine the playtest
simulator runs at a thousand times real speed, and a copy of what the arbiter
runs in the installation (src/arbiter_mesh/gamestate.cpp). This file only
drives it in real time: a thread lets the clock run so timers, `idle`,
`every` and `at` steps fire when they are due, and events and pulls go in as
they arrive. Format: game_state_machine/GAME_FORMAT.md.

overlord.py uses it with --game, for dry runs without an arbiter: events come
from the socket (send.sh) or the keyboard simulator (simulate.py), and the
cues the game fires play here. `set:` changes device settings, which only the
arbiter can deliver; here they are logged, so a dry run shows what the room
would do.
"""

import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from playtest import spec  # noqa: E402
from playtest.engine import Engine  # noqa: E402

DEFAULT_GAME = ROOT / "game_state_machine" / "state_config.yaml"

# Every event the event sources may send. A state that does not list one ignores it.
GAME_EVENTS = spec.KNOWN_EVENTS

TICK = 0.05   # seconds between clock steps: timers fire within this of their time


class Game:
    def __init__(self, path, fire_cue, roped=None):
        """fire_cue: callback(cue_name, enter) that plays one cue; enter is
        True for a cue fired on entering a state."""
        self.cfg = spec.load(path)
        self.engine = Engine(self.cfg, roped=roped)
        self.name = self.engine.name
        self.states = self.cfg["states"]
        self._fire_cue = fire_cue
        self._lock = threading.RLock()
        self._t0 = None
        self._running = False
        self._listeners = []
        self.engine.on_record(self._on_record)

    # -- what the rest of overlord asks --------------------------------------------
    @property
    def state(self):
        return self.engine.state

    @property
    def decay_rate(self):
        return self.engine.decay_rate

    @property
    def charge_per_pull(self):
        return self.engine.charge_per_pull

    @property
    def settings(self):
        return self.engine.settings

    @property
    def timer_deadline(self):
        """Monotonic time the current `after` fires, or None."""
        due = self.engine.after_due
        return None if due is None or self._t0 is None else self._t0 + due

    def on_record(self, fn):
        """fn(record) for everything the engine does (playtest/engine.py lists
        the kinds): the play log listens here in --game mode."""
        self._listeners.append(fn)

    def handles(self, event_name):
        if event_name == "enter":
            return False
        return event_name in GAME_EVENTS or any(
            event_name in spec.reactions(s) for s in self.states.values())

    def reacts_to(self, event_name):
        """True if the current state lists this event."""
        if event_name == "enter":
            return False
        with self._lock:
            return event_name in spec.reactions(self.states.get(self.state) or {})

    # -- driving it ----------------------------------------------------------------
    def start(self):
        print(f"game: {self.name}, starting in {self.cfg['start']}", flush=True)
        with self._lock:
            self._t0 = time.monotonic()
            self.engine.start()
        self._running = True
        threading.Thread(target=self._clock, daemon=True).start()

    def stop(self):
        self._running = False

    def event(self, event_name):
        """A game event or device input by name, now."""
        with self._lock:
            self._catch_up()
            self.engine.input(event_name)

    def pull(self, channel):
        """A pull on a rope channel, now (the simulator's keys)."""
        with self._lock:
            self._catch_up()
            self.engine.pull(channel)

    def goto(self, state):
        """Force the machine into a state, as if a transition led there."""
        with self._lock:
            self._catch_up()
            self.engine._enter(state)

    def set_param(self, name, value):
        with self._lock:
            return self.engine.set_param(name, value)

    def _catch_up(self):
        if self._t0 is not None:
            self.engine.advance_to(time.monotonic() - self._t0)

    def _clock(self):
        while self._running:
            with self._lock:
                self._catch_up()
            time.sleep(TICK)

    def _on_record(self, rec):
        kind = rec["kind"]
        if kind == "state":
            print(f"game: {rec.get('prev') or '-'} -> {rec['name']}", flush=True)
        elif kind == "event":
            print(f"game: {rec['name']} {'in' if rec['reacted'] else 'ignored in'} {self.state}",
                  flush=True)
        elif kind == "set":
            print(f"game: set {rec['key']}={rec['value']}", flush=True)
        elif kind == "goals":
            print(f"game: goals {', '.join(rec['colors'])}", flush=True)
        elif kind == "timer":
            print(f"game: {rec['name']} in {self.state}", flush=True)
        elif kind == "input":
            print(f"game: input {rec['name']}", flush=True)
        if kind == "cue":
            self._fire_cue(rec["name"], rec["enter"])
        for fn in self._listeners:
            fn(rec)
