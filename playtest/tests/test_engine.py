"""The behaviours the Python engine and the arbiter's C++ both promise.

    python -m unittest discover -s playtest/tests -t .      (from the repo root)

Each test is a small game and a script of pulls and waits. Where the arbiter
must do the same thing, the test says so; src/arbiter_mesh/gamestate.cpp and
components.cpp are the other implementation.
"""

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import game_to_json  # noqa: E402

from playtest import spec  # noqa: E402
from playtest.engine import Engine  # noqa: E402

RED, BLUE, GREEN, YELLOW, ORANGE, PURPLE_CH = 1, 2, 3, 4, 5, 6   # default palette


def game(states, **top):
    cfg = {
        "game": "t",
        "start": next(iter(states)),
        "components": {"major": ["red", "blue", "green", "yellow", "orange"]},
        "cues": {"game": ["game_reset_components", "game_randomize_simon_2_goals"],
                 "audio_once": ["audio_once_a", "audio_once_b"]},
        "states": states,
    }
    cfg.update(top)
    errors = game_to_json.validate(cfg)
    if errors:
        raise AssertionError("invalid test game: " + "; ".join(errors))
    return cfg


def run(cfg, seed=1, **kw):
    e = Engine(cfg, seed=seed, **kw)
    log = []
    e.on_record(log.append)
    e.start()
    return e, log


def kinds(log, kind):
    return [r for r in log if r["kind"] == kind]


def pulls(e, ch, n, every=0.25):
    for _ in range(n):
        e.pull(ch)
        e.advance(every)


class Charge(unittest.TestCase):
    def setUp(self):
        self.cfg = game({"s": {"on": {"nongoal_component_activated": {"cue": ["audio_once_a"]},
                                      "component_deactivated": {"cue": ["audio_once_b"]}}}})

    def test_six_quick_pulls_light_five_do_not(self):
        e, log = run(self.cfg)
        pulls(e, RED, 5)
        self.assertFalse(e.lit_channel(RED))
        e.pull(RED)
        self.assertTrue(e.lit_channel(RED))

    def test_lit_until_drained_to_zero(self):
        e, log = run(self.cfg)
        pulls(e, RED, 6, every=0.0)          # 100% at t=0
        e.advance(14.9)
        self.assertTrue(e.lit_channel(RED))
        e.advance(0.2)                        # 15 s at 6.6667 %/s
        self.assertFalse(e.lit_channel(RED))
        dark = [r for r in kinds(log, "event") if r["name"] == "component_deactivated"]
        self.assertAlmostEqual(dark[0]["t"], 15.0, places=2)

    def test_ropes_of_one_colour_share_a_component(self):
        e, _ = run(self.cfg)
        for ch in (RED, RED + 6, RED + 12):    # 1, 7, 13 are all red
            pulls(e, ch, 2, every=0.0)
        self.assertTrue(e.lit_channel(RED + 6))


class Simon(unittest.TestCase):
    def test_goals_lit_and_others_dark_wins(self):
        cfg = game({
            "round": {"on": {"enter": {"goals": 2},
                             "all_components_activated": {"goto": "won"}}},
            "won": {},
        })
        e, log = run(cfg, seed=3)
        goals = e.goal_colors()
        self.assertEqual(len(goals), 2)
        other = next(c for c in spec.GAME1_COLORS if c not in goals)
        pulls(e, 1 + spec.COLORS.index(other), 6, every=0.0)   # a non-goal lit
        for c in goals:
            pulls(e, 1 + spec.COLORS.index(c), 6, every=0.0)
        self.assertEqual(e.state, "round")                       # blocked by the non-goal
        e.advance(15.1)                                          # everything drains; goals too
        for c in goals:
            pulls(e, 1 + spec.COLORS.index(c), 6, every=0.0)
        self.assertEqual(e.state, "won")

    def test_legacy_randomize_cue_still_deals(self):
        cfg = game({"s": {"on": {"enter": {"cue": ["game_randomize_simon_2_goals"]}}}})
        e, log = run(cfg)
        self.assertEqual(len(e.goal_colors()), 2)
        self.assertEqual(len(kinds(log, "goals")), 1)

    def test_new_goals_never_repeat_the_last_set(self):
        cfg = game({"s": {"on": {"enter": {"goals": 1}, "pull": {"goto": "s"}}}})
        e, log = run(cfg, seed=5)
        dealt = [tuple(r["colors"]) for r in kinds(log, "goals")]
        for _ in range(30):
            e.pull(RED)
            dealt.append(tuple(kinds(log, "goals")[-1]["colors"]))
        self.assertTrue(all(a != b for a, b in zip(dealt, dealt[1:])))


class ShippedGame(unittest.TestCase):
    def test_passive_starts_again_after_a_hard_round(self):
        # A hard round deals all five colours as goals and nothing clears them,
        # so back in passive every activation is a goal activation. The game
        # once listened only for a non-goal one there and could never restart.
        cfg = spec.load(os.path.join(ROOT, "game_state_machine", "state_config.yaml"))
        e, _ = run(cfg)
        e.deal_goals(5)
        self.assertEqual(e.state, "passive")
        pulls(e, RED, 6, every=0.0)
        self.assertEqual(e.state, "easy_round")


class PullEvents(unittest.TestCase):
    def test_pull_events_come_before_the_charge(self):
        # A pull that starts the game lands its charge in the new round, after
        # the round's reset -- not before it, where the reset would wipe it.
        cfg = game({
            "idle": {"on": {"pull": {"goto": "round"}}},
            "round": {"on": {"enter": {"cue": ["game_reset_components"]}}},
        })
        e, _ = run(cfg)
        e.pull(RED)
        self.assertEqual(e.state, "round")
        self.assertAlmostEqual(e.charge_of_channel(RED), 20.0)

    def test_kinds_of_pull(self):
        cfg = game({"s": {"on": {"enter": {"goals": 1}}}})
        e, log = run(cfg, seed=2)
        goal = spec.COLORS.index(e.goal_colors()[0])
        other = next(c for c in range(5) if c != goal)
        for ch in (1 + goal, 1 + other, PURPLE_CH):
            e.pull(ch)
        self.assertEqual([r["cls"] for r in kinds(log, "pull")], ["goal", "nongoal", "final"])

    def test_a_pull_is_activity_for_idle(self):
        cfg = game({"s": {"idle": {"delay": "10s", "cue": ["audio_once_a"]}}})
        e, log = run(cfg)
        for _ in range(4):
            e.advance(8)
            e.pull(RED)                       # a pull that lights nothing still counts
        self.assertEqual(kinds(log, "cue"), [])
        e.advance(10.1)
        self.assertEqual(len(kinds(log, "cue")), 1)


class Counters(unittest.TestCase):
    def test_reach_fires_once_on_the_way_up(self):
        cfg = game({"play": {"on": {"pull": {"add": {"hits": 1}},
                                    "hits_reached": {"cue": ["audio_once_a"]}}}},
                   counters={"hits": {"reach": "$need"}}, params={"need": 3})
        e, log = run(cfg)
        for _ in range(5):
            e.pull(RED)
            e.advance(0.1)
        self.assertEqual(e.counters["hits"], 5)
        self.assertEqual([r["t"] for r in kinds(log, "cue")], [0.2])   # the third pull
        e.react({"clear": ["hits"]})
        for _ in range(3):
            e.pull(RED)
        self.assertEqual(len(kinds(log, "cue")), 2)                     # climbing again counts again

    def test_reaction_still_fires_its_cues_before_the_reach_moves_on(self):
        cfg = game({
            "s": {"on": {"pull": {"add": {"n": 1}, "cue": ["audio_once_a"], "goto": "t"},
                         "n_reached": {"goto": "won"}}},
            "t": {}, "won": {},
        }, counters={"n": {"reach": 1}})
        e, log = run(cfg)
        e.pull(RED)
        self.assertEqual(e.state, "won")
        self.assertEqual([r["name"] for r in kinds(log, "cue")], ["audio_once_a"])


class Timers(unittest.TestCase):
    def test_every_repeats_whatever_happens(self):
        cfg = game({"s": {"every": {"delay": "1.5s", "cue": ["audio_once_a"]}}})
        e, log = run(cfg)
        for _ in range(6):
            e.advance(1.0)
            e.pull(RED)
        self.assertEqual([r["t"] for r in kinds(log, "cue")], [1.5, 3.0, 4.5, 6.0])

    def test_after_takes_a_param_and_live_changes_apply_on_next_entry(self):
        cfg = game({"a": {"after": {"delay": "$wait", "goto": "b"}},
                    "b": {"after": {"delay": "1s", "goto": "a"}}},
                   params={"wait": {"value": "10s", "min": "5s", "max": "20s"}})
        e, log = run(cfg)
        e.set_param("wait", 99)             # clamped to 20
        self.assertEqual(e.params["wait"].value, 20)
        e.advance(10.0)                      # the running timer kept its 10 s
        self.assertEqual(e.state, "b")
        e.advance(1.0 + 19.9)
        self.assertEqual(e.state, "a")
        e.advance(0.2)
        self.assertEqual(e.state, "b")

    def test_at_steps_run_once(self):
        cfg = game({"s": {"at": {"2s": {"cue": ["audio_once_a"]}, "0.5s": {"cue": ["audio_once_b"]}}}})
        e, log = run(cfg)
        e.advance(10)
        self.assertEqual([(r["t"], r["name"]) for r in kinds(log, "cue")],
                         [(0.5, "audio_once_b"), (2.0, "audio_once_a")])


class Colors(unittest.TestCase):
    def test_shuffle_moves_game1_and_leaves_purple(self):
        cfg = game({"s": {"on": {"pull": {"colors": "shuffle"}}},
                    "t": {"on": {"enter": {"colors": "restore"}}}})
        e, log = run(cfg)
        before = list(e.palette)
        e.pull(RED)
        self.assertNotEqual(e.palette, before)
        for b, a in zip(before, e.palette):
            self.assertEqual(b == spec.PURPLE, a == spec.PURPLE)
        self.assertEqual(e.stored_palette, before)
        e.react({"colors": "restore"})
        self.assertEqual(e.palette, before)


class Validation(unittest.TestCase):
    def errors(self, states, **top):
        cfg = {"game": "t", "start": next(iter(states)),
               "components": {"major": ["red", "blue", "green", "yellow", "orange"]},
               "cues": {"audio_once": ["audio_once_a"]}, "states": states, **top}
        return game_to_json.validate(cfg)

    def test_unknown_param(self):
        self.assertTrue(self.errors({"s": {"after": {"delay": "$nope", "goto": "s"}}}))

    def test_count_param_in_a_delay(self):
        e = self.errors({"s": {"after": {"delay": "$n", "goto": "s"}}}, params={"n": 3})
        self.assertTrue(any("duration" in x for x in e))

    def test_reached_needs_a_reach(self):
        e = self.errors({"s": {"on": {"n_reached": {"goto": "s"}}}}, counters={"n": None})
        self.assertTrue(any("reach" in x for x in e))

    def test_no_goto_in_every(self):
        e = self.errors({"s": {"every": {"delay": "1s", "goto": "s"}}})
        self.assertTrue(any("no goto" in x for x in e))

    def test_goals_range(self):
        e = self.errors({"s": {"on": {"enter": {"goals": "$g"}}}},
                        params={"g": {"value": 2, "min": 1, "max": 7}})
        self.assertTrue(any("goals" in x for x in e))

    def test_decimal_durations(self):
        self.assertEqual(self.errors({"s": {"after": {"delay": "1.5s", "goto": "s"}}}), [])

    def test_shipped_games_are_valid(self):
        folder = os.path.join(ROOT, "game_state_machine")
        paths = [os.path.join(folder, "state_config.yaml")] + [
            os.path.join(folder, "games", f) for f in sorted(os.listdir(os.path.join(folder, "games")))
            if f.endswith(".yaml")]
        for p in paths:
            self.assertEqual(game_to_json.validate(spec.load(p)), [], p)


if __name__ == "__main__":
    unittest.main()
