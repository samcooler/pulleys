"""python -m playtest <command> ...  (run from the repo root; needs pyyaml)

  sim GAME            play simulated visitors through a game and summarise
  compare V1 V2 ...   the same visitors through several variants, side by side
  tune GAME           find the param values simulated visitors enjoy most
  validate GAME       check the fun score tracks the simulator's ground truth
  live-sim GAME       the live director learning over a simulated night
  analyse LOG         sessions and fun scores of a play log (real or simulated)
  calibrate LOG ...   fit the simulated crowd to real play logs

A variant is a game file, optionally with param values and options after a
colon:  game_state_machine/state_config.yaml:round_time=90s,show_goals=1
"""

import argparse
import json
import os
import statistics
import sys
from collections import Counter, defaultdict

from . import spec
from .calibrate import calibrate, load_population, save_population
from .metrics import analyse, correlation, summarise
from .players import Population
from .tuner import BIG_GAP, prior_from, tune
from .world import World

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GAME = os.path.join(ROOT, "game_state_machine", "state_config.yaml")


def parse_value(text):
    try:
        return spec.parse_duration(text)
    except ValueError:
        pass
    try:
        return int(text)
    except ValueError:
        return float(text)


def parse_variant(text):
    """PATH[:k=v,k=v] -> (label, cfg, params, options)."""
    path, _, rest = text.partition(":")
    if not os.path.exists(path) and os.path.exists(os.path.join(ROOT, path)):
        path = os.path.join(ROOT, path)
    cfg = load_game(path)
    params, options = {}, {}
    for item in filter(None, rest.split(",")):
        k, _, v = item.partition("=")
        if k == "show_goals":
            options["show_goals"] = v not in ("0", "false", "no")
        elif k == "clip":
            options["clip_seconds"] = parse_value(v)
        else:
            params[k] = parse_value(v)
    label = os.path.basename(path).rsplit(".", 1)[0] + (f" [{rest}]" if rest else "")
    return label, cfg, params, options


def load_game(path):
    cfg = spec.load(path)
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import game_to_json
    errors = game_to_json.validate(cfg)
    if errors:
        sys.exit(f"{path}: {len(errors)} problem(s)\n  " + "\n  ".join(errors))
    return cfg


def population_arg(args):
    return load_population(args.population) if getattr(args, "population", None) else Population()


def table(rows, cols):
    widths = [max(len(c), *(len(str(r[i])) for r in rows)) for i, c in enumerate(cols)]
    line = lambda vals: "  ".join(str(v).rjust(w) if i else str(v).ljust(w)
                                  for i, (v, w) in enumerate(zip(vals, widths)))
    print(line(cols))
    print(line(["-" * w for w in widths]))
    for r in rows:
        print(line(r))


def run_episodes(cfg, params, options, n, seed, population):
    rows, outcomes, traces = [], [], []
    for i in range(n):
        w = World(cfg, seed=seed + i, params=params, population=population, **options)
        o = w.run_episode()
        ms = analyse(w.trace, cfg, gap=BIG_GAP)
        if ms:
            ms[0]["true_fun"] = o["fun"]
            ms[0]["archetype"] = o["archetype"]
            rows.append(ms[0])
        outcomes.append(o)
        traces.append(w.trace)
    return rows, outcomes, traces


def describe(label, rows, outcomes, cfg):
    s = summarise(rows)
    order = list(cfg["states"])
    print(f"\n{label}: {len(outcomes)} visits")
    print(f"  stayed {s.get('minutes', 0):.1f} min, {s.get('pull_rate', 0) * 60:.0f} pulls/min, "
          f"{s.get('ropes', 0):.1f} ropes, progress {s.get('progress', 0):.2f}, "
          f"rounds won {s.get('wins', 0):.2f}, timed out {s.get('timeouts', 0):.2f}")
    print(f"  pulls: goal {s.get('goal_share', 0):.0%}, other colours {s.get('nongoal_share', 0):.0%}, "
          f"purple {s.get('final_share', 0):.0%}")
    print(f"  fun score {s.get('fun', 0):.2f}   (simulated true fun {s.get('true_fun', 0):.1f})")
    reached = s.get("reached", {})
    print("  reached: " + ", ".join(f"{st} {reached.get(st, 0):.0%}" for st in order))
    by = defaultdict(list)
    for r in rows:
        by[r["archetype"]].append(r)
    out = []
    for k, rs in sorted(by.items()):
        why = Counter(x for o in outcomes if o["archetype"] == k for x in o["reasons"])
        out.append([k, len(rs), f"{statistics.fmean(r['minutes'] for r in rs):.1f}",
                    f"{statistics.fmean(r['progress'] for r in rs):.2f}",
                    f"{statistics.fmean(r['fun'] for r in rs):.2f}",
                    f"{statistics.fmean(r['true_fun'] for r in rs):.1f}",
                    ", ".join(f"{a} {b}" for a, b in why.most_common(2))])
    print()
    table(out, ["who", "n", "min", "progress", "fun", "true fun", "why they left"])


def cmd_sim(args):
    label, cfg, params, options = parse_variant(args.game)
    pop = population_arg(args)
    if args.hours:
        w = World(cfg, seed=args.seed, params=params, population=pop, **options)
        outs = w.run_night(hours=args.hours, groups_per_hour=args.rate)
        rows = analyse(w.trace, cfg)
        s = summarise(rows)
        print(f"{label}: {args.hours:g} h, {len(outs)} groups, {s['sessions']} sessions")
        for k in ("minutes", "pull_rate", "ropes", "progress", "wins", "timeouts", "fun"):
            print(f"  {k:<10} {s.get(k, 0):.2f}")
        print("  reached: " + ", ".join(f"{st} {s['reached'].get(st, 0):.0%}" for st in cfg["states"]))
        if args.log:
            write_log(args.log, w.trace)
        return
    rows, outcomes, traces = run_episodes(cfg, params, options, args.episodes, args.seed, pop)
    describe(label, rows, outcomes, cfg)
    if args.log:
        with open(args.log, "w") as f:
            offset = 0.0
            for i, tr in enumerate(traces):
                for r in tr:
                    f.write(json.dumps({**r, "t": round(r["t"] + offset, 3), "episode": i}) + "\n")
                offset += (tr[-1]["t"] if tr else 0) + 600
        print(f"\nwrote {args.log}")


def write_log(path, trace):
    with open(path, "w") as f:
        for r in trace:
            f.write(json.dumps(r) + "\n")
    print(f"wrote {path}")


def cmd_compare(args):
    pop = population_arg(args)
    results = []
    for v in args.variants:
        label, cfg, params, options = parse_variant(v)
        rows, outcomes, _ = run_episodes(cfg, params, options, args.episodes, args.seed, pop)
        s = summarise(rows)
        reached_end = max((s["reached"].get(st, 0) for st in list(cfg["states"])[-3:]), default=0)
        results.append([label, f"{s['minutes']:.1f}", f"{s['progress']:.2f}", f"{s['wins']:.2f}",
                        f"{s['timeouts']:.2f}", f"{reached_end:.0%}", f"{s['fun']:.2f}",
                        f"{s.get('true_fun', 0):.1f}"])
        if args.verbose:
            describe(label, rows, outcomes, cfg)
    print()
    table(results, ["variant", "min", "progress", "wins", "timeouts", "late states", "fun", "true fun"])


def cmd_tune(args):
    label, cfg, params, options = parse_variant(args.game)
    result = tune(cfg, episodes=args.episodes, k=args.levels, rounds=args.rounds,
                  seed=args.seed, population=population_arg(args), options=options)
    result["prior"] = prior_from(result)
    allp = spec.load_params(cfg)
    print("\nsuggested values (paste into the game's params):")
    for name, v in result["best"].items():
        p = allp[name]
        mark = "" if v == p.default else f"   (was {p.show(p.default)})"
        print(f"  {name}: {p.show(v)}{mark}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=1)
        print(f"\nwrote {args.out} (its `prior` seeds the live director)")


def cmd_validate(args):
    """Across varied settings and crowds, does the observable score follow the
    simulator's ground truth?"""
    import random
    label, cfg, _, options = parse_variant(args.game)
    allp = {n: p for n, p in spec.load_params(cfg).items() if p.tunable}
    rng = random.Random(args.seed)
    pop = population_arg(args)
    xs, ys, per = [], [], defaultdict(list)
    for i in range(args.episodes):
        params = {n: p.clamp(rng.uniform(p.lo, p.hi)) for n, p in allp.items()}
        rows, outs, _ = run_episodes(cfg, params, options, 1, args.seed + i, pop)
        if not rows:
            continue
        r = rows[0]
        xs.append(r["fun"])
        ys.append(r["true_fun"])
        for k in ("minutes", "progress", "wins", "sustain", "pull_rate", "lulls"):
            per[k].append(float(r[k]))
    print(f"{label}: {len(xs)} visits with random settings")
    print(f"  fun score vs simulated true fun: r = {correlation(xs, ys):.2f}")
    for k, vals in per.items():
        print(f"  {k:<10} vs true fun: r = {correlation(vals, ys):+.2f}")


def cmd_live_sim(args):
    """The live director over a simulated night, against the same night on
    the defaults."""
    from .live import simulate
    label, cfg, _, options = parse_variant(args.game)
    prior = None
    if args.prior:
        with open(args.prior) as f:
            prior = json.load(f)["prior"]
    pop = population_arg(args)
    base, _ = simulate(cfg, args.hours, args.rate, args.seed, population=pop,
                       options=options, director=False)
    extra = {"focus": args.focus.split(",")} if args.focus else {}
    live, d = simulate(cfg, args.hours, args.rate, args.seed, prior=prior, population=pop,
                       options=options, **extra)
    def by_quarter(rows):
        q = args.hours * 3600 / 4
        return [statistics.fmean([f for t, f, _ in rows if i * q <= t < (i + 1) * q] or [0])
                for i in range(4)]
    print(f"{label}: {args.hours:g} simulated hours, {args.rate:g} groups/h")
    print(f"  sessions: {len(base)} on defaults, {len(live)} with the director")
    print("  mean fun by quarter of the night:")
    print("    defaults  " + "  ".join(f"{x:5.2f}" for x in by_quarter(base)))
    print("    director  " + "  ".join(f"{x:5.2f}" for x in by_quarter(live)))
    print(f"  overall: defaults {statistics.fmean(f for _, f, _ in base):.2f}, "
          f"director {statistics.fmean(f for _, f, _ in live):.2f}")
    s = d.summary()
    print("  where the director ended up:")
    for name, cur in s["current"].items():
        best = max(s["levels"][name], key=lambda r: r["mean_fun"])
        tried = ", ".join(f"{r['value']}:{r['sessions']:.0f}" for r in s["levels"][name])
        print(f"    {name:<16} now {cur:>6}   best estimate {best['value']:>6} "
              f"(fun {best['mean_fun']:.2f})   sessions per level {tried}")


def read_log(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def cmd_analyse(args):
    cfg = load_game(args.game)
    records = []
    for p in args.logs:
        records += read_log(p)
    records.sort(key=lambda r: r["t"])
    rows = analyse(records, cfg, gap=args.gap)
    out = [[f"{r['start']:.0f}", f"{r['minutes']:.1f}", r["pulls"], r["ropes"],
            f"{r['progress']:.2f}", r["wins"], r["timeouts"], f"{r['final_share']:.0%}",
            f"{r['fun']:.2f}"] for r in rows]
    if out:
        table(out, ["start s", "min", "pulls", "ropes", "progress", "wins", "timeouts", "purple", "fun"])
    s = summarise(rows)
    print(f"\n{s['sessions']} sessions; mean fun {s.get('fun', 0):.2f}, "
          f"{s.get('minutes', 0):.1f} min, progress {s.get('progress', 0):.2f}")
    params = [r for r in records if r["kind"] == "param"]
    if params:
        print(f"{len(params)} param changes logged; the last: "
              + ", ".join(f"{r['name']}={r['value']}" for r in params[-5:]))


def cmd_calibrate(args):
    cfg = load_game(args.game)
    records = []
    for p in args.logs:
        records += read_log(p)
    records.sort(key=lambda r: r["t"])
    rows = analyse(records, cfg)
    hours = args.hours or max(1e-3, (records[-1]["t"] - records[0]["t"]) / 3600.0)
    pop, report = calibrate(cfg, rows, hours, iterations=args.iterations, seed=args.seed)
    save_population(pop, args.out)
    print(f"wrote {args.out}; about {report['groups_per_hour']:.0f} groups an hour; simulated vs real:")
    for k in report["target"]:
        if k != "sessions":
            print(f"  {k:<18} {report['fitted'][k]:8.2f}  {report['target'][k]:8.2f}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m playtest", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, episodes=200):
        p.add_argument("--episodes", type=int, default=episodes, help="visits to simulate")
        p.add_argument("--seed", type=int, default=0)
        p.add_argument("--population", help="a population file from calibrate")

    p = sub.add_parser("sim", help="simulate visitors playing a game")
    p.add_argument("game", nargs="?", default=DEFAULT_GAME)
    common(p)
    p.add_argument("--hours", type=float, help="a night of overlapping groups instead of single visits")
    p.add_argument("--rate", type=float, default=20.0, help="groups an hour, with --hours")
    p.add_argument("--log", help="write the play log (JSONL)")
    p.set_defaults(fn=cmd_sim)

    p = sub.add_parser("compare", help="several variants, same visitors")
    p.add_argument("variants", nargs="+")
    common(p)
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("tune", help="search the tunable params")
    p.add_argument("game", nargs="?", default=DEFAULT_GAME)
    common(p, episodes=120)
    p.add_argument("--levels", type=int, default=5)
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--out", help="write the result JSON (its prior seeds the live director)")
    p.set_defaults(fn=cmd_tune)

    p = sub.add_parser("validate", help="does the fun score follow the simulated truth?")
    p.add_argument("game", nargs="?", default=DEFAULT_GAME)
    common(p, episodes=300)
    p.set_defaults(fn=cmd_validate)

    p = sub.add_parser("live-sim", help="the live director over a simulated night")
    p.add_argument("game", nargs="?", default=DEFAULT_GAME)
    p.add_argument("--hours", type=float, default=12.0)
    p.add_argument("--rate", type=float, default=20.0, help="groups an hour")
    p.add_argument("--prior", help="a tune --out result to start from")
    p.add_argument("--focus", help="only these params, comma-separated")
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--population", help="a population file from calibrate")
    p.set_defaults(fn=cmd_live_sim)

    p = sub.add_parser("analyse", help="sessions and scores of play logs")
    p.add_argument("logs", nargs="+")
    p.add_argument("--game", default=DEFAULT_GAME)
    p.add_argument("--gap", type=float, default=40.0)
    p.set_defaults(fn=cmd_analyse)

    p = sub.add_parser("calibrate", help="fit the simulated crowd to real logs")
    p.add_argument("logs", nargs="+")
    p.add_argument("--game", default=DEFAULT_GAME)
    p.add_argument("--hours", type=float, help="hours the logs cover (default: first to last record)")
    p.add_argument("--iterations", type=int, default=40)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--out", default="population.json")
    p.set_defaults(fn=cmd_calibrate)

    args = ap.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()
