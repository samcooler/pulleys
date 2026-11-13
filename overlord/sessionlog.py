"""The activity log: sessions of play, and every activation inside them.

A session starts when the game leaves its idle state (the game file's `start`
state, passive) and ends when it comes back to it. Each line of activity.jsonl,
next to the play logs, is one terse event with `t` in wall-clock seconds:

    {"t":..,"e":"start","n":7,"state":"easy_round"}   session 7 began
    {"t":..,"e":"pull","n":7,"ch":3,"c":"red","g":"goal"}   an activation (g: goal|nongoal|final)
    {"t":..,"e":"state","n":7,"s":"medium_round"}     the game moved to another phase
    {"t":..,"e":"end","n":7,"why":"idle"}             idle again (or "reset", "restart")

Sessions are rebuilt by replaying that file, so the live view and the history
are one code path. Phase rank is the state's position in the game file, so the
"maximum phase" is the furthest one reached, not the last.
"""

import json
import threading
import time
from pathlib import Path

DEFAULT_IDLE = "passive"


class SessionLog:
    def __init__(self, path, idle_state=DEFAULT_IDLE, phases=(), clock=time.time):
        """phases: the game's state names in order (idle first), for ranking."""
        self.path = Path(path)
        self.idle = idle_state
        self.order = {name: i for i, name in enumerate(phases)}
        self.clock = clock
        self.lock = threading.Lock()
        self.sessions = []   # finished sessions, oldest first
        self.current = None
        self.state = None
        self._next = 1
        self._file = None
        self._warned = False
        self._replay()

    # -- rebuilding ------------------------------------------------------------------
    def _replay(self):
        try:
            lines = self.path.read_text().splitlines()
        except OSError:
            return
        for line in lines:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            self._apply(ev)
            self._next = max(self._next, ev.get("n", 0) + 1)
        # The app went away mid-session; the first state the game reports now settles it.

    def _rank(self, state):
        return self.order.get(state, len(self.order))

    def _apply(self, ev):
        kind, t = ev.get("e"), ev.get("t", 0)
        cur = self.current
        if kind == "start":
            cur = self.current = {"n": ev["n"], "start": t, "end": None, "pulls": 0,
                                  "goal_pulls": 0, "max_phase": ev["state"],
                                  "phases": [[ev["state"], t]]}
        elif cur is None:
            return
        elif kind == "pull":
            cur["pulls"] += 1
            cur["goal_pulls"] += ev.get("g") == "goal"
        elif kind == "state":
            cur["phases"].append([ev["s"], t])
            if self._rank(ev["s"]) > self._rank(cur["max_phase"]):
                cur["max_phase"] = ev["s"]
        elif kind == "end":
            cur["end"], cur["why"] = t, ev.get("why", "idle")
            self.sessions.append(cur)
            self.current = None

    def _emit(self, ev):
        ev = {"t": round(self.clock(), 3), **ev}
        self._apply(ev)
        try:
            if self._file is None:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                self._file = open(self.path, "a", buffering=1)
            self._file.write(json.dumps(ev, separators=(",", ":")) + "\n")
        except OSError as e:
            if not self._warned:
                print(f"sessionlog: cannot write to {self.path}: {e}", flush=True)
                self._warned = True

    # -- from the play log -----------------------------------------------------------------
    def feed(self, rec):
        """A play log record (playlog.PlayLog.listen)."""
        kind = rec.get("kind")
        with self.lock:
            if kind == "state":
                self._enter(rec["name"])
            elif kind == "start" and self.current:
                self._emit({"e": "end", "n": self.current["n"], "why": "restart"})
                self.state = None
            elif kind == "pull" and self.current:
                self._emit({"e": "pull", "n": self.current["n"], "ch": rec["ch"],
                            "c": rec["color"], "g": rec["cls"]})

    def _enter(self, state):
        if state == self.state:
            return
        self.state = state
        cur = self.current
        if state == self.idle:
            if cur:
                self._emit({"e": "end", "n": cur["n"], "why": "idle"})
        elif cur is None:
            n, self._next = self._next, self._next + 1
            self._emit({"e": "start", "n": n, "state": state})
        elif cur["phases"][-1][0] != state:
            self._emit({"e": "state", "n": cur["n"], "s": state})

    # -- what the dashboard asks ---------------------------------------------------------------
    def snapshot(self, limit=200):
        """Newest first. Each row has the figures the Sessions tab shows."""
        now = self.clock()
        with self.lock:
            rows = list(self.sessions)
            if self.current:
                rows.append(self.current)
            out, prev_end = [], None
            for s in rows:
                end = s["end"] or now
                out.append({
                    "n": s["n"], "start": s["start"], "end": s["end"], "live": s["end"] is None,
                    "duration": round(end - s["start"], 1),
                    "gap": None if prev_end is None else round(max(0, s["start"] - prev_end), 1),
                    "pulls": s["pulls"], "goal_pulls": s["goal_pulls"],
                    "max_phase": s["max_phase"], "why": s.get("why"),
                    "phases": [[name, round(((s["phases"][i + 1][1] if i + 1 < len(s["phases"]) else end)
                                             - at), 1)] for i, (name, at) in enumerate(s["phases"])],
                })
                prev_end = s["end"]
            return {"idle_state": self.idle, "state": self.state, "now": now,
                    "order": sorted(self.order, key=self.order.get),
                    "total": len(out), "sessions": out[::-1][:limit]}

    def close(self):
        with self.lock:
            if self._file:
                self._file.close()
                self._file = None
