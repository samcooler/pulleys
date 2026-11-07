"""The live director: learns, from real visitors, which settings they enjoy.

Runs inside overlord on the Pi (`overlord.py --director`). It watches the play
log as it is written, and each time a session of play ends -- nobody has
pulled for `gap` seconds -- it scores that session (metrics.fun_score), credits
the score to the settings that were live during it, and picks the settings for
whoever comes next. It sends them to the arbiter through the bridge
(GAME-PARAM lines); the arbiter clamps every value to the range the game file
gives, so the director can never take the game anywhere its designer did not
allow.

How it chooses: Thompson sampling, one param at a time. Each tunable param is
cut into a few levels; each level keeps a running estimate of the fun score
sessions got under it. To choose, it draws a plausible score for every level
from what it knows (wide when it knows little, narrow when it knows a lot) and
takes the best draw. So it mostly plays what has worked and keeps trying the
rest in proportion to how possibly-better they still look. Old evidence is
discounted (`discount` per session), so it follows the crowd as the night
changes, and a prior from the simulator (playtest/tuner.py) gives it a
sensible place to start without outweighing a handful of real sessions.

Settings only change between sessions, never under someone's hands.
"""

import json
import math
import os
import random
import threading
import time

from . import spec
from .metrics import GAP, GameShape, fun_score, session_metrics
from .tuner import levels


class Director:
    def __init__(self, cfg, send, state_path=None, prior=None, k=5, discount=0.995,
                 noise=None, prior_weight=2.0, gap=GAP, seed=None, frozen=False,
                 focus=None, min_spread=0.1, log=print):
        """send(name, value): deliver one param to the arbiter.

        Which params it turns: `focus` if given; else, with a prior, those
        whose levels made a difference of at least `min_spread` in simulation
        (a night is a few hundred sessions -- not enough to learn every knob
        at once, so it spends them on the ones that matter); else all."""
        self.cfg = cfg
        self.shape = GameShape(cfg)
        tunable = {n: p for n, p in spec.load_params(cfg).items() if p.tunable}
        if focus:
            keep = set(focus)
        elif prior:
            keep = {n for n, by in prior.items()
                    if n in tunable and by and max(by.values()) - min(by.values()) >= min_spread}
        else:
            keep = set(tunable)
        self.params = {n: p for n, p in tunable.items() if n in keep}
        if len(self.params) > 3:
            log(f"director: turning {len(self.params)} knobs at once; with session scores this "
                "noisy it will learn slowly (focus on one or two)")
        self.arms = {n: levels(p, k) for n, p in self.params.items()}
        self.send = send
        self.state_path = state_path
        self.discount = discount
        self.noise = noise            # None: estimated from the sessions seen
        self.rewards = []
        self.gap = gap
        self.frozen = frozen
        self.log = log
        self.rng = random.Random(seed)
        self.lock = threading.RLock()
        # per param, per level: [weight, mean] of real sessions only
        self.stats = {n: [[0.0, 0.0] for _ in vals] for n, vals in self.arms.items()}
        # The simulator's view, as how much better or worse each level is than
        # the param's other levels -- not as absolute scores: a simulated visit
        # and a real stretch of overlapping play score on different scales,
        # but which level beats which should carry over.
        self.prior_off = {n: [0.0] * len(vals) for n, vals in self.arms.items()}
        self.prior_w = prior_weight if prior else 0.0
        if prior:
            self._apply_prior(prior)
        self.choice = {n: p.default for n, p in self.params.items()}
        self.history = []
        self.on_session = []     # fn(entry) for each session scored: its fun, its settings
        self.records = []        # since the last session closed, plus context
        self.state = None
        self.last_pull = None
        self.session_start = None
        self.sent_at = None
        self._load()

    # -- learning ---------------------------------------------------------------
    def _apply_prior(self, prior):
        for name, by_level in prior.items():
            if name not in self.arms or not by_level:
                continue
            centre = sum(by_level.values()) / len(by_level)
            for i, v in enumerate(self.arms[name]):
                near = min(by_level, key=lambda k: abs(float(k) - v))
                self.prior_off[name][i] = float(by_level[near]) - centre

    def _baseline(self):
        r = self.rewards[-200:]
        return sum(r) / len(r) if r else 0.0

    def estimate(self, name, i):
        """(mean, weight) for one level: real sessions, plus the prior's
        offset around the running average, weighted as prior_w sessions."""
        w, mean = self.stats[name][i]
        base = self._baseline()
        m = base + (self.prior_w * self.prior_off[name][i] + w * (mean - base)) / (self.prior_w + w) \
            if self.prior_w + w > 0 else base
        return m, self.prior_w + w

    def choose(self):
        """Draw settings for the next session."""
        if self.frozen:
            return dict(self.choice)
        pick = {}
        noise = self.noise
        if noise is None:
            r = self.rewards[-200:]
            noise = (sum((x - sum(r) / len(r)) ** 2 for x in r) / (len(r) - 1)) ** 0.5 \
                if len(r) >= 10 else 1.0
        for name, vals in self.arms.items():
            draws = []
            for i in range(len(vals)):
                mean, w = self.estimate(name, i)
                draws.append(self.rng.gauss(mean, noise / math.sqrt(w + 1e-3)))
            pick[name] = vals[max(range(len(vals)), key=lambda i: draws[i])]
        return pick

    def credit(self, setting, reward):
        for name, value in setting.items():
            if name not in self.arms:
                continue
            for s in self.stats[name]:
                s[0] *= self.discount
            i = min(range(len(self.arms[name])), key=lambda j: abs(self.arms[name][j] - value))
            w, mean = self.stats[name][i]
            self.stats[name][i] = [w + 1.0, mean + (reward - mean) / (w + 1.0)]

    # -- the log ------------------------------------------------------------------
    def feed(self, rec):
        """Every record overlord's play log writes, as it writes it."""
        with self.lock:
            if rec["kind"] == "pull":
                if self.session_start is None:
                    self.session_start = rec["t"]
                self.last_pull = rec["t"]
            if rec["kind"] == "start":
                # the arbiter (re)loaded its game: it is back on the defaults
                self.sent_at = None
            self.records.append(rec)

    def tick(self, now):
        """Call about once a second with the play log's clock."""
        with self.lock:
            if self.sent_at is None and self.session_start is None:
                self._apply(self.choose())
                return
            if self.session_start is None or now - self.last_pull < self.gap:
                return
            self._close(now)

    def _close(self, now):
        start, end = self.session_start, self.last_pull
        before = [r for r in self.records if r["t"] < start]
        during = [r for r in self.records if start <= r["t"] <= end + self.gap]
        m = session_metrics({"start": start, "end": end, "records": during}, self.shape, before)
        reward = fun_score(m)
        self.rewards.append(reward)
        self.credit(self.choice, reward)
        self.history.append({"at": time.time(), "minutes": round(m["minutes"], 2),
                             "pulls": m["pulls"], "progress": round(m["progress"], 2),
                             "wins": m["wins"], "fun": round(reward, 3), "params": dict(self.choice)})
        self.history = self.history[-500:]
        for fn in self.on_session:
            fn(self.history[-1])
        self.log(f"director: session {m['minutes']:.1f} min, {m['pulls']} pulls, "
                 f"progress {m['progress']:.2f} -> fun {reward:.2f}")
        # keep only the state the next session starts in
        state_recs = [r for r in self.records if r["kind"] in ("state", "goals", "palette")]
        self.records = state_recs[-3:]
        self.session_start = self.last_pull = None
        self._apply(self.choose())
        self._save()

    def _apply(self, setting):
        changed = {n: v for n, v in setting.items() if v != self.choice.get(n) or self.sent_at is None}
        self.choice = dict(setting)
        for name, value in changed.items():
            self.send(name, value)
        self.sent_at = time.time()
        if changed:
            self.log("director: next session " + ", ".join(
                f"{n}={self.params[n].show(v)}" for n, v in changed.items()))

    # -- what it knows ----------------------------------------------------------------
    def summary(self):
        with self.lock:
            out = {"frozen": self.frozen, "sessions": len(self.history), "current": {}, "levels": {}}
            for name, vals in self.arms.items():
                p = self.params[name]
                out["current"][name] = p.show(self.choice[name])
                out["levels"][name] = []
                for i, v in enumerate(vals):
                    m, w = self.estimate(name, i)
                    out["levels"][name].append({"value": p.show(v), "sessions": round(self.stats[name][i][0], 1),
                                                "weight": round(w, 2), "mean_fun": round(m, 3)})
            out["recent"] = self.history[-10:]
            return out

    def _save(self):
        if not self.state_path:
            return
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"arms": self.arms, "stats": self.stats, "choice": self.choice,
                       "rewards": self.rewards[-200:], "history": self.history}, f)
        os.replace(tmp, self.state_path)

    def _load(self):
        if not self.state_path or not os.path.exists(self.state_path):
            return
        try:
            with open(self.state_path) as f:
                saved = json.load(f)
        except (OSError, ValueError) as e:
            self.log(f"director: could not read {self.state_path}: {e}")
            return
        for name, vals in saved.get("arms", {}).items():
            # only keep what still matches the game's ranges
            if self.arms.get(name) == vals:
                self.stats[name] = saved["stats"][name]
                if name in saved.get("choice", {}):
                    self.choice[name] = saved["choice"][name]
        self.history = saved.get("history", [])
        self.rewards = saved.get("rewards", [])
        self.log(f"director: resumed with {len(self.history)} sessions of experience")


def simulate(cfg, hours=12.0, groups_per_hour=20.0, seed=0, prior=None, population=None,
             options=None, director=True, log=lambda m: None, **kw):
    """A simulated night with the director in the loop, as it would run on the
    Pi: it sees the play log as the night goes, and its settings reach the
    game between sessions. Returns ([(t, fun, settings)] per session it scored,
    the director) -- or, with director=False, the same night's sessions on the
    defaults."""
    from .metrics import analyse
    from .world import World

    w = World(cfg, seed=seed, population=population, **(options or {}))
    if not director:
        w.run_night(hours, groups_per_hour)
        return [(r["start"], r["fun"], {}) for r in analyse(w.trace, cfg)], None
    d = Director(cfg, send=lambda n, v: w.engine.set_param(n, v), seed=seed,
                 prior=prior, log=log, **kw)
    out = []
    d.on_session.append(lambda h: out.append((w.t, h["fun"], dict(h["params"]))))
    w.engine.on_record(d.feed)
    w.run_night(hours, groups_per_hour, each_second=lambda: d.tick(w.t))
    return out, d
