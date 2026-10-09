"""Game engine: runs a state machine from game_state_machine/*.yaml.

Game events come in (from serial, the socket, or the simulator), the current
state reacts by firing cues through overlord's dispatch and maybe moving to
another state. Format: game_state_machine/GAME_FORMAT.md.
"""

import re
import threading
import time
from pathlib import Path

import yaml

DEFAULT_GAME = Path(__file__).resolve().parent.parent / "game_state_machine" / "state_config.yaml"

# Every event the event sources may send. A state that does not list one ignores it.
GAME_EVENTS = {
    "component_activated",
    "component_deactivated",
    "all_components_activated",
    "all_components_deactivated",
    "final_component_activated",
    "final_component_deactivated",
    "all_final_components_activated",
}


def parse_duration(text):
    """`10s` or `2m` -> seconds."""
    m = re.fullmatch(r"\s*(\d+)\s*(s|m)\s*", str(text))
    if not m:
        raise ValueError(f"bad duration {text!r}; use e.g. 10s or 2m")
    return int(m.group(1)) * (60 if m.group(2) == "m" else 1)


def reactions(state):
    # YAML 1.1 reads a bare `on:` key as boolean True
    return state.get("on") or state.get(True) or {}


class Game:
    def __init__(self, path, fire_cue):
        """fire_cue: callback(cue_name) that plays one cue."""
        with open(path) as f:
            config = yaml.safe_load(f)
        self.name = config.get("game", Path(path).stem)
        self.start_state = config["start"]
        self.states = config["states"]
        self._fire_cue = fire_cue
        self._lock = threading.RLock()
        self._timer = None
        self.state = None
        self.timer_deadline = None  # monotonic time the current `after` fires, or None

        for name, state in self.states.items():
            targets = [a.get("goto") for a in reactions(state).values() if a]
            if state.get("after"):
                parse_duration(state["after"]["delay"])
                targets.append(state["after"]["goto"])
            for t in targets:
                if t and t not in self.states:
                    raise ValueError(f"state {name} goes to unknown state {t}")

    def handles(self, event_name):
        return event_name in GAME_EVENTS or any(
            event_name in reactions(s) for s in self.states.values())

    def reacts_to(self, event_name):
        """True if the current state lists this event."""
        with self._lock:
            return event_name in reactions(self.states.get(self.state, {}))

    def start(self):
        print(f"game: {self.name}, starting in {self.start_state}", flush=True)
        with self._lock:
            self._enter(self.start_state)

    def event(self, event_name):
        with self._lock:
            listed = reactions(self.states[self.state])
            if event_name not in listed:
                print(f"game: {event_name} ignored in {self.state}", flush=True)
                return
            action = listed[event_name] or {}
            print(f"game: {event_name} in {self.state}", flush=True)
            self._fire(action.get("cue"))
            if action.get("goto"):
                self._enter(action["goto"])

    def stop(self):
        with self._lock:
            self._cancel_timer()

    def _enter(self, name):
        self._cancel_timer()
        print(f"game: {self.state or '-'} -> {name}", flush=True)
        self.state = name
        state = self.states[name]
        self._fire(state.get("on_enter_cue"))
        after = state.get("after")
        if after:
            seconds = parse_duration(after["delay"])
            timer = threading.Timer(seconds, lambda: self._timeout(timer, after["goto"]))
            timer.daemon = True
            self._timer = timer
            self.timer_deadline = time.monotonic() + seconds
            timer.start()

    def _timeout(self, timer, to_state):
        with self._lock:
            # A timer cancelled just as it fired must not move a state it no longer owns.
            if self._timer is not timer:
                return
            print(f"game: timer in {self.state}", flush=True)
            self._enter(to_state)

    def _cancel_timer(self):
        if self._timer:
            self._timer.cancel()
        self._timer = None
        self.timer_deadline = None

    def _fire(self, cues):
        for cue in cues or []:
            self._fire_cue(cue)
