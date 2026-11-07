"""Fit the simulated crowd to real play logs.

The visitor model's numbers start as guesses (playtest/players.py). Once the
installation has logged real nights, this finds the population whose
simulated nights look most like them: how long sessions last, how fast people
pull, how many ropes are in use, how far they get, how much purple they pull.
It scales the archetypes' trait means (pull rate, boredom, time budget, ...)
by a random search, scoring each candidate by how far its simulated session
statistics sit from the real ones.

The result is a population file the simulator and tuner take with
--population, so tuning in simulation is tuning for the people who actually
come.
"""

import json
import random
import statistics

from .metrics import analyse
from .players import Population
from .world import World

# Which trait means may be scaled, and how far.
SCALABLE = {
    "pull_rate": (0.4, 2.5),
    "boredom": (0.3, 3.0),
    "budget": (0.3, 3.0),
    "interest0": (0.6, 1.3),
    "curiosity": (0.4, 1.8),
    "compliance": (0.4, 1.6),
}

# Session statistics to match, and how much each counts.
TARGETS = {
    "minutes_median": 1.0,
    "minutes_p75": 0.6,
    "pull_rate_median": 1.0,
    "ropes_median": 0.6,
    "progress_mean": 0.8,
    "final_share_mean": 0.5,
}


def stats(rows):
    if not rows:
        return None
    mins = sorted(r["minutes"] for r in rows)
    return {
        "minutes_median": statistics.median(mins),
        "minutes_p75": mins[int(0.75 * (len(mins) - 1))],
        "pull_rate_median": statistics.median(r["pull_rate"] for r in rows),
        "ropes_median": statistics.median(r["ropes"] for r in rows),
        "progress_mean": statistics.fmean(r["progress"] for r in rows),
        "final_share_mean": statistics.fmean(r["final_share"] for r in rows),
        "sessions": len(rows),
    }


def distance(a, b):
    """Weighted squared relative error between two stats dicts."""
    d = 0.0
    for k, w in TARGETS.items():
        scale = max(abs(b[k]), 0.05)
        d += w * ((a[k] - b[k]) / scale) ** 2
    return d


def simulate_stats(cfg, population, hours, groups_per_hour, seeds, params=None):
    rows = []
    for s in seeds:
        w = World(cfg, seed=s, population=population, params=params)
        w.run_night(hours=hours, groups_per_hour=groups_per_hour)
        rows += analyse(w.trace, cfg)
    return stats(rows)


def calibrate(cfg, real_rows, hours_observed, iterations=40, seed=0, params=None, log=print):
    """real_rows: analyse() of the real logs. Returns (Population, report)."""
    target = stats(real_rows)
    if target is None or target["sessions"] < 5:
        raise ValueError("need at least 5 real sessions to calibrate against")
    # Groups that overlap play as one session, so sessions an hour is a floor
    # on groups an hour, not the number: how many more is fitted too.
    sessions_per_hour = max(1.0, target["sessions"] / max(hours_observed, 0.1))
    log(f"real: {target['sessions']} sessions over {hours_observed:.1f} h "
        f"({sessions_per_hour:.1f}/h); " +
        ", ".join(f"{k} {v:.2f}" for k, v in target.items() if k != "sessions"))
    rng = random.Random(seed)
    seeds = [seed + i for i in range(3)]
    best = Population()
    best_crowding = 1.5
    best_stats = simulate_stats(cfg, best, 1.0, sessions_per_hour * best_crowding, seeds, params)
    best_d = distance(best_stats, target)
    log(f"  start   distance {best_d:.3f}")
    for i in range(iterations):
        # mostly small steps from the best so far, sometimes a jump
        jump = rng.random() < 0.25
        scale = {}
        for k, (lo, hi) in SCALABLE.items():
            cur = best.scale.get(k, 1.0)
            v = rng.uniform(lo, hi) if jump else cur * rng.lognormvariate(0, 0.2)
            scale[k] = min(hi, max(lo, v))
        crowding = rng.uniform(1.0, 4.0) if jump else best_crowding * rng.lognormvariate(0, 0.15)
        crowding = min(4.0, max(1.0, crowding))
        cand = Population(weights=dict(best.weights), scale=scale)
        st = simulate_stats(cfg, cand, 1.0, sessions_per_hour * crowding, seeds, params)
        if st is None:
            continue
        d = distance(st, target)
        if d < best_d:
            best, best_stats, best_d, best_crowding = cand, st, d, crowding
            log(f"  step {i + 1:>3} distance {d:.3f}  groups/h {sessions_per_hour * crowding:.1f}  " +
                ", ".join(f"{k}x{v:.2f}" for k, v in scale.items()))
    report = {"target": target, "fitted": best_stats, "distance": best_d,
              "groups_per_hour": sessions_per_hour * best_crowding, "population": best.as_json()}
    return best, report


def save_population(pop, path):
    with open(path, "w") as f:
        json.dump(pop.as_json(), f, indent=2)


def load_population(path):
    with open(path) as f:
        return Population.from_json(json.load(f))
