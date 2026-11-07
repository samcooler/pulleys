"""Find the param values simulated visitors enjoy most.

Every candidate setting is played by the same simulated groups (common random
numbers: episode i always brings the same people with the same dice), so a
difference between two settings is the settings, not the luck of the draw.

    1. Sweep: each tunable param across its levels, the others at their
       defaults. Shows which knobs matter at all, and which way.
    2. Climb: from the best of the sweep, coordinate ascent -- one param at a
       time to its best level -- until a full pass changes nothing.

The objective is the observable fun score (playtest/metrics.py), the same
number the live director optimises on real visitors, so what is tuned here is
a fair prior for what will be tuned live. The simulator's own ground-truth fun
is reported alongside, as a check.
"""

import math
import statistics

from . import spec
from .metrics import analyse
from .world import World

BIG_GAP = 1e9   # an episode is one visit: one session, however long its pauses


def levels(p, k=5):
    """k values spread over a param's range, its default among them."""
    vals = [p.lo + (p.hi - p.lo) * i / (k - 1) for i in range(k)]
    vals.append(p.default)
    vals = sorted({p.clamp(v) for v in vals})
    return vals


def evaluate(cfg, params, episodes=120, seed=0, population=None, show_goals=None, clip_seconds=None):
    """Play `episodes` visits with these params. Returns a dict of means."""
    funs, true, mins, prog, wins = [], [], [], [], []
    for i in range(episodes):
        extra = {} if clip_seconds is None else {"clip_seconds": clip_seconds}
        w = World(cfg, seed=seed + i, params=params, population=population,
                  show_goals=show_goals, **extra)
        w.run_episode()
        rows = analyse(w.trace, cfg, gap=BIG_GAP)
        truth = [r for r in w.trace if r["kind"] == "truth" and r["what"] == "leave"]
        if not rows:
            funs.append(0.0)
            mins.append(0.0)
            prog.append(0.0)
            wins.append(0)
        else:
            r = rows[0]
            funs.append(r["fun"])
            mins.append(r["minutes"])
            prog.append(r["progress"])
            wins.append(r["wins"])
        true.append(truth[0]["fun"] if truth else 0.0)
    n = len(funs)
    return {
        "fun": statistics.fmean(funs),
        "fun_se": statistics.stdev(funs) / math.sqrt(n) if n > 1 else 0.0,
        "true_fun": statistics.fmean(true),
        "minutes": statistics.fmean(mins),
        "progress": statistics.fmean(prog),
        "wins": statistics.fmean(wins),
        "episodes": n,
    }


def tune(cfg, episodes=120, k=5, rounds=3, seed=0, population=None, options=None, log=print):
    """Returns {"baseline", "sweep", "best", "best_score", "params"}.
    options: World options (show_goals, clip_seconds)."""
    options = options or {}
    params = {n: p for n, p in spec.load_params(cfg).items() if p.tunable}
    if not params:
        raise ValueError("this game has no tunable params (a param needs a min and a max)")
    defaults = {n: p.default for n, p in params.items()}
    cache = {}

    def score(setting):
        key = tuple(sorted(setting.items()))
        if key not in cache:
            cache[key] = evaluate(cfg, setting, episodes, seed, population, **options)
        return cache[key]

    base = score(defaults)
    log(f"baseline fun {base['fun']:.3f} ±{base['fun_se']:.3f}  "
        f"(true {base['true_fun']:.1f}, {base['minutes']:.1f} min, progress {base['progress']:.2f})")

    sweep = {}
    for name, p in params.items():
        row = []
        for v in levels(p, k):
            s = score({**defaults, name: v})
            row.append((v, s))
        sweep[name] = row
        best_v, best_s = max(row, key=lambda vs: vs[1]["fun"])
        spread = max(s["fun"] for _, s in row) - min(s["fun"] for _, s in row)
        log(f"  {name:<20} best {p.show(best_v):>6} ({best_s['fun']:.3f})   spread {spread:.3f}   "
            + "  ".join(f"{p.show(v)}:{s['fun']:.2f}" for v, s in row))

    # Start the climb from each param's best sweep level, kept only if it helps.
    current = dict(defaults)
    for name, row in sweep.items():
        v, _ = max(row, key=lambda vs: vs[1]["fun"])
        trial = {**current, name: v}
        if score(trial)["fun"] > score(current)["fun"]:
            current = trial
    for r in range(rounds):
        changed = False
        for name, p in params.items():
            best_v, best_f = current[name], score(current)["fun"]
            for v in levels(p, k):
                f = score({**current, name: v})["fun"]
                if f > best_f + 1e-9:
                    best_v, best_f = v, f
            if best_v != current[name]:
                current = {**current, name: best_v}
                changed = True
        log(f"climb pass {r + 1}: fun {score(current)['fun']:.3f}  "
            + ", ".join(f"{n}={params[n].show(v)}" for n, v in current.items() if v != defaults[n]))
        if not changed:
            break

    best = score(current)
    # Re-check the winner on fresh visitors: the climb picked the best of many
    # noisy scores, so its score on the seeds it was chosen on is optimistic.
    fresh = evaluate(cfg, current, episodes, seed + 100000, population, **options)
    fresh_base = evaluate(cfg, defaults, episodes, seed + 100000, population, **options)
    log(f"best fun {best['fun']:.3f} on the tuning visitors; on fresh ones "
        f"{fresh['fun']:.3f} vs defaults {fresh_base['fun']:.3f}")
    return {
        "baseline": base,
        "best": current,
        "best_score": best,
        "fresh": {"best": fresh, "defaults": fresh_base},
        "sweep": {n: [{"value": v, **s} for v, s in row] for n, row in sweep.items()},
        "params": {n: {"default": p.default, "min": p.lo, "max": p.hi, "kind": p.kind}
                   for n, p in params.items()},
    }


def prior_from(result):
    """What the live director should believe before it has seen anyone: each
    level's simulated fun score (playtest/live.py weighs it lightly)."""
    return {name: {str(r["value"]): r["fun"] for r in rows}
            for name, rows in result["sweep"].items()}
