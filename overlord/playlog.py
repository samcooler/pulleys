"""The play log: every pull and everything the game did, one JSON line each.

What real visitors do is the data the game gets tuned on (playtest/README.md).
The bridge already hands overlord every pull ("EV <ch> ...") and every cue the
arbiter fires ("cue <name>"); this keeps them instead of dropping them, in the
same record shape the playtest simulator writes, so `python -m playtest
analyse` reads a real night exactly as it reads a simulated one.

The arbiter also reports what the game does, as MESH_MSG_GAME_INFO, which
the bridge passes on as a line of key=value items:

    GI state=easy_round goals=red-blue count.score=3

    start=<game>             it (re)loaded a game: params back to their defaults
    state=<state>            the game moved to a state
    goals=<c>-<c>            the goal colours it dealt (goals=none)
    palette=<32 digits>      the colour index each rope channel wears
    count.<name>=<n>         a counter's new value
    input=<device.input>     a device's input reached the game (diorama.button_pressed)
    param.<name>=<milli>     a param's value, in thousandths
    hb.state= hb.goals= hb.palette=
                             a heartbeat every 15 s: what is true now, so a log
                             that started late catches up. Recorded only where
                             it differs from what the log thought.

With those it can say of each pull which colour it was and whether that
colour was a goal. Before the arbiter sends them (older firmware) the log
still has every pull and cue, and assumes the default palette.

Files are one a day, play-YYYY-MM-DD.jsonl, under $OVERLORD_PLAYLOG, else
/var/lib/overlord/playlog when that is writable, else overlord/playlog/.
`t` is wall-clock seconds, so logs from several days sort together.
"""

import json
import os
import threading
import time
from pathlib import Path

COLORS = ("red", "blue", "green", "yellow", "orange", "purple")
PURPLE = 5
CHANNELS = 32


def default_dir():
    if os.environ.get("OVERLORD_PLAYLOG"):
        return Path(os.environ["OVERLORD_PLAYLOG"])
    var = Path("/var/lib/overlord")
    if var.is_dir() and os.access(var, os.W_OK):
        return var / "playlog"
    return Path(__file__).resolve().parent / "playlog"


class PlayLog:
    def __init__(self, directory=None, clock=time.time):
        self.dir = Path(directory) if directory else default_dir()
        self.clock = clock
        self.lock = threading.Lock()
        self.listeners = []
        self.palette = [(ch + len(COLORS) - 1) % len(COLORS) for ch in range(CHANNELS)]
        self.palette_known = False
        self.goals = set()
        self.state = None
        self.records = 0
        self._file = None
        self._day = None
        self._warned = False

    def listen(self, fn):
        """fn(record) for every record, after it is written."""
        self.listeners.append(fn)

    # -- from the bridge ---------------------------------------------------------
    def line(self, text):
        """One line from the bridge. Returns the last record it made, or None."""
        text = text.strip()
        if text.startswith("EV "):
            return self._pull(text)
        if text.startswith("GI "):
            rec = None
            for item in text[3:].split():
                key, _, value = item.partition("=")
                rec = self._info(key, value) or rec
            return rec
        if text.startswith("cue "):
            parts = text.split()
            if len(parts) >= 2:
                return self.cue(parts[1], enter=len(parts) > 2 and parts[2] == "enter")
        return None

    def _pull(self, text):
        try:
            ch, mode, mag = (int(x) for x in text.split()[1:4])
        except ValueError:
            return None
        if not 0 <= ch < CHANNELS:
            return None
        color = self.palette[ch]
        cls = "final" if color == PURPLE else ("goal" if COLORS[color] in self.goals else "nongoal")
        rec = {"kind": "pull", "ch": ch, "color": COLORS[color], "cls": cls, "mag": mag}
        if not self.palette_known:
            rec["palette"] = "assumed"
        return self.write(rec)

    def cue(self, name, enter=False):
        """A cue the arbiter fired."""
        return self.write({"kind": "cue", "name": name, "enter": enter})

    def _info(self, key, value):
        """One key=value item of the arbiter's game telemetry."""
        sync = key.startswith("hb.")
        if sync:
            key = key[3:]
        if key == "start":
            self.state = None
            return self.write({"kind": "start", "game": value})
        if key == "state" and value:
            if sync and value == self.state:
                return None
            prev, self.state = self.state, value
            rec = {"kind": "state", "name": value, "prev": prev}
            return self.write({**rec, "sync": True} if sync else rec)
        if key == "goals":
            goals = set() if value == "none" else set(value.split("-"))
            if sync and goals == self.goals:
                return None
            self.goals = goals
            rec = {"kind": "goals", "colors": [c for c in COLORS if c in goals]}
            return self.write({**rec, "sync": True} if sync else rec)
        if key == "palette" and len(value) == CHANNELS and value.isdigit():
            palette = [min(int(d), PURPLE) for d in value]
            known = self.palette_known
            self.palette_known = True
            if palette == self.palette and known:
                return None
            self.palette = palette
            return self.write({"kind": "palette", "colors": [COLORS[c] for c in palette]})
        if key == "input" and value:
            return self.write({"kind": "input", "name": value})
        if key.startswith(("count.", "param.")):
            what, _, name = key.partition(".")
            try:
                v = int(value)
            except ValueError:
                return None
            if what == "count":
                return self.write({"kind": "count", "name": name, "value": v})
            return self.write({"kind": "param", "name": name, "value": v / 1000.0})
        return None

    # -- from overlord's own engine (--game) ----------------------------------------
    def engine_record(self, rec):
        """A record from overlord's own engine in --game mode, already shaped."""
        rec = {k: v for k, v in rec.items() if k != "t"}
        if rec["kind"] == "goals":
            self.goals = set(rec["colors"])
        elif rec["kind"] == "state":
            self.state = rec["name"]
        elif rec["kind"] == "palette":
            self.palette = [COLORS.index(c) for c in rec["colors"]]
        return self.write(rec)

    # -- the file ------------------------------------------------------------------------
    def write(self, rec):
        now = self.clock()
        rec = {"t": round(now, 3), **rec}
        with self.lock:
            try:
                day = time.strftime("%Y-%m-%d", time.localtime(now))
                if day != self._day:
                    if self._file:
                        self._file.close()
                    self.dir.mkdir(parents=True, exist_ok=True)
                    self._file = open(self.dir / f"play-{day}.jsonl", "a", buffering=1)
                    self._day = day
                self._file.write(json.dumps(rec, separators=(",", ":")) + "\n")
                self.records += 1
            except OSError as e:
                if not self._warned:
                    print(f"playlog: cannot write to {self.dir}: {e}", flush=True)
                    self._warned = True
        for fn in self.listeners:
            try:
                fn(rec)
            except Exception as e:  # a listener's bug must not stop the log
                print(f"playlog: listener failed: {e}", flush=True)
        return rec

    def path(self):
        return str(self.dir / f"play-{self._day}.jsonl") if self._day else str(self.dir)

    def close(self):
        with self.lock:
            if self._file:
                self._file.close()
                self._file = None
