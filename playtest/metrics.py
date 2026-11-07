"""From a play log to sessions, and from a session to a fun score.

A play log is a list of records (playtest/engine.py lists the kinds): the
simulator writes them, and so does overlord from the real installation
(overlord/playlog.py). This module only uses what the real room can observe --
pulls, states, cues, goals -- never the simulator's `truth` records, so the
score it gives a simulated night is the one it would give a real one.

Sessions
    A session is a run of pulls with no gap longer than `gap` seconds (40 by
    default). With one group at a time that is a visit; when groups overlap it
    is a stretch of continuous play, which is what the installation itself
    experiences.

Fun score
    Nobody can measure fun from a pulley. What the room can see is revealed
    engagement: people stay when it is good, push on when they are getting
    somewhere, keep their pace up when they are hooked, and walk off when it
    stings. FUN_WEIGHTS turns those into one number; a session that held people
    five minutes and got through two rounds scores about 3. The simulator, which
    does know each visitor's enjoyment, checks the score tracks it
    (`python -m playtest validate`).
"""

import math
from collections import Counter

from .spec import reactions

GAP = 40.0

FUN_WEIGHTS = {
    "dwell": 1.0,       # per doubling of minutes stayed: log2(1 + minutes)
    "progress": 1.0,    # how far through the game they got, 0-1
    "advances": 0.6,    # rounds won, up to 4, scaled 0-1
    "sustain": 0.0,     # late pull rate vs early, 0-1. Off: a party or victory
                        # needs no pulling, so the best sessions slow down at
                        # the end, and in simulation this ran against fun.
    "setback": -0.6,    # walked off within 15 s of a round timing out
}


class GameShape:
    """What a play log alone cannot say about a game: which transitions are
    wins and which are timeouts, and how far along each state is. Progress
    is a state's distance from the start, unless the game lists its
    `milestones` (states in the order they mark getting further)."""

    def __init__(self, cfg):
        self.start = cfg["start"]
        self.order = list(cfg["states"])
        self.timeout_edges = set()    # time ran out in a state that could be won
        self.scheduled_edges = set()  # a state that only ever moves on by its timer
        self.win_edges = set()
        for name, st in cfg["states"].items():
            st = st or {}
            wins = {(name, a["goto"]) for a in reactions(st).values() if a and a.get("goto")}
            self.win_edges |= wins
            if st.get("after"):
                edge = (name, st["after"]["goto"])
                (self.timeout_edges if wins else self.scheduled_edges).add(edge)
        # Progress: shortest path from the start through wins, normalised.
        depth = {self.start: 0}
        frontier = [self.start]
        while frontier:
            nxt = []
            for s in frontier:
                for a, b in sorted(self.win_edges | self.timeout_edges | self.scheduled_edges):
                    if a == s and b not in depth:
                        depth[b] = depth[s] + 1
                        nxt.append(b)
            frontier = nxt
        top = max(depth.values()) or 1
        self.depth = {s: d / top for s, d in depth.items()}
        # A game that is not a straight road (bop-it's game_over is as far
        # from the start as its win) names the states that mean progress.
        milestones = cfg.get("milestones")
        if milestones:
            self.depth = {s: (i + 1) / len(milestones) for i, s in enumerate(milestones)}

    def is_win(self, prev, name):
        """A move the players earned: an event's goto, out of the start state
        or onward. Leaving the start state counts as getting the game going."""
        return (prev, name) in self.win_edges and (prev, name) not in self.timeout_edges

    def is_timeout(self, prev, name):
        return (prev, name) in self.timeout_edges and (prev, name) not in self.win_edges


def sessions(records, gap=GAP):
    """Split a log into sessions of continuous play. Each is a dict with the
    session's records (pulls and everything that happened between its first
    pull and gap seconds after its last)."""
    pulls = [r["t"] for r in records if r["kind"] == "pull"]
    if not pulls:
        return []
    spans = []
    start = last = pulls[0]
    for t in pulls[1:]:
        if t - last > gap:
            spans.append((start, last))
            start = t
        last = t
    spans.append((start, last))
    out = []
    i = 0
    for a, b in spans:
        recs = []
        while i < len(records) and records[i]["t"] < a:
            i += 1
        j = i
        while j < len(records) and records[j]["t"] <= b + gap:
            recs.append(records[j])
            j += 1
        out.append({"start": a, "end": b, "records": recs})
    return out


def session_metrics(sess, shape, context=None):
    """Observable measures of one session. `context`: the records before it,
    so the state it began in is known."""
    recs = sess["records"]
    a, b = sess["start"], sess["end"]
    dur = max(0.0, b - a)
    pulls = [r for r in recs if r["kind"] == "pull" and r["t"] <= b]
    n = len(pulls)
    cls = Counter(r.get("cls") for r in pulls)
    channels = {r["ch"] for r in pulls}

    state = None
    for r in context or []:
        if r["kind"] == "state":
            state = r["name"]
    depth = shape.depth.get(state, 0.0) if state else 0.0
    wins = timeouts = 0
    last_timeout = None
    states = []
    for r in recs:
        if r["kind"] == "state" and r["t"] <= b + 1:
            prev = r.get("prev") or state
            if shape.is_win(prev, r["name"]):
                wins += 1
            elif shape.is_timeout(prev, r["name"]):
                timeouts += 1
                last_timeout = r["t"]
            state = r["name"]
            states.append(state)
            depth = max(depth, shape.depth.get(state, 0.0))

    third = dur / 3 if dur > 0 else 0
    early = sum(1 for r in pulls if r["t"] <= a + third) if third else n
    late = sum(1 for r in pulls if r["t"] >= b - third) if third else n
    sustain = min(1.0, late / early) if early else 0.0
    setback = last_timeout is not None and 0 <= b - last_timeout <= 15.0
    lulls = sum(1 for p, q in zip(pulls, pulls[1:]) if q["t"] - p["t"] >= 10.0)
    cues = [r for r in recs if r["kind"] == "cue" and not r["name"].startswith("game_") and r["t"] <= b]
    inputs = Counter(r["name"] for r in recs if r["kind"] == "input" and r["t"] <= b + 1)
    return {
        "start": a,
        "minutes": dur / 60.0,
        "pulls": n,
        "pull_rate": n / dur if dur > 0 else 0.0,
        "ropes": len(channels),
        "goal_share": cls.get("goal", 0) / n if n else 0.0,
        "nongoal_share": cls.get("nongoal", 0) / n if n else 0.0,
        "final_share": cls.get("final", 0) / n if n else 0.0,
        "progress": depth,
        "wins": wins,
        "timeouts": timeouts,
        "sustain": sustain,
        "setback": setback,
        "lulls": lulls,
        "cues": len(cues),
        "dropped_clips": sum(1 for r in cues if r.get("dropped")),
        "states": states,
        "inputs": dict(inputs),   # devices: diorama.door_opened, diorama.button_pressed, ...
    }


def fun_score(m, weights=FUN_WEIGHTS):
    return (weights["dwell"] * math.log2(1 + m["minutes"])
            + weights["progress"] * m["progress"]
            + weights["advances"] * min(m["wins"], 4) / 4
            + weights["sustain"] * m["sustain"]
            + weights["setback"] * (1.0 if m["setback"] else 0.0))


def analyse(records, cfg, gap=GAP):
    """Every session in a log, with its measures and fun score."""
    shape = GameShape(cfg)
    out = []
    before = []
    idx = 0
    for s in sessions(records, gap):
        while idx < len(records) and records[idx]["t"] < s["start"]:
            before.append(records[idx])
            idx += 1
        m = session_metrics(s, shape, before)
        m["fun"] = fun_score(m)
        truth = [r for r in s["records"] if r["kind"] == "truth" and r["what"] == "leave"]
        if truth:
            m["true_fun"] = sum(r["fun"] for r in truth) / len(truth)
        out.append(m)
    return out


def summarise(rows):
    """Means of the session measures, and how many sessions reached each state."""
    if not rows:
        return {"sessions": 0}
    keys = ["minutes", "pulls", "pull_rate", "ropes", "goal_share", "nongoal_share",
            "final_share", "progress", "wins", "timeouts", "sustain", "lulls", "fun"]
    out = {"sessions": len(rows)}
    for k in keys:
        out[k] = sum(float(r[k]) for r in rows) / len(rows)
    out["setback_rate"] = sum(1 for r in rows if r["setback"]) / len(rows)
    reached = Counter(s for r in rows for s in set(r["states"]))
    out["reached"] = {s: n / len(rows) for s, n in reached.most_common()}
    used = Counter(name for r in rows for name in r.get("inputs", {}))
    out["inputs"] = {name: n / len(rows) for name, n in used.most_common()}
    if any("true_fun" in r for r in rows):
        tf = [r for r in rows if "true_fun" in r]
        out["true_fun"] = sum(r["true_fun"] for r in tf) / len(tf)
    return out


def correlation(xs, ys):
    n = len(xs)
    if n < 3:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return float("nan")
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)
