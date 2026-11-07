"""The arbiter's C++ engine and playtest/engine.py, side by side.

Builds src/arbiter_mesh/gamestate.cpp and components.cpp for this computer
(playtest/host/) and plays the same scenarios through both engines. Each
scenario reads what it needs (which goals were dealt) from the engine it is
driving, so the engines' different dice do not matter; what they observe must
come out the same, and match what the scenario expects.

    python -m unittest playtest.tests.test_conformance

Skips if there is no C++ compiler, or no ArduinoJson (a `pio run -e
arbiter_mesh` puts it in .pio/libdeps/).
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import game_to_json  # noqa: E402

from playtest import spec  # noqa: E402
from playtest.engine import Engine  # noqa: E402

JSON_LIB = os.path.join(ROOT, ".pio", "libdeps", "arbiter_mesh", "ArduinoJson", "src")
COLORS = spec.COLORS


def compiler():
    for cc in ("clang++", "g++", "c++"):
        if shutil.which(cc):
            return cc
    return None


class Python:
    """playtest/engine.py behind the scenarios' interface."""
    name = "python"

    def load(self, cfg, path):
        self.e = Engine(cfg, seed=1)
        self.cues = []
        self.e.on_record(lambda r: self.cues.append(r["name"]) if r["kind"] == "cue" else None)
        self.e.start()

    def pull(self, ch):
        self.e.pull(ch)

    def wait(self, s):
        self.e.advance(s)

    def input(self, name):
        self.e.input(name)

    def param(self, name, value):
        try:
            self.e.set_param(name, value)
            return True
        except KeyError:
            return False

    def state(self):
        return self.e.state

    def goals(self):
        return self.e.goal_colors()

    def take_cues(self):
        out, self.cues = self.cues, []
        return out


class Cpp:
    """The arbiter's engine, built for this computer, over a pipe."""
    name = "c++"

    def __init__(self, binary):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.cues = []

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)

    def _do(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            got = self.p.stdout.readline()
            if not got:
                raise RuntimeError(f"engine_host died on {line!r}")
            got = got.rstrip("\n")
            if got == ".":
                return out
            if got.startswith("cue "):
                self.cues.append(got.split()[1])
            elif got.startswith("err "):
                raise AssertionError(got)
            out.append(got)

    def load(self, cfg, path):
        self.cues = []
        self._do(f"load {path} 1")

    def pull(self, ch):
        self._do(f"pull {ch}")

    def wait(self, s):
        self._do(f"wait {int(round(s * 1000))}")

    def input(self, name):
        self._do(f"input {name}")

    def param(self, name, value):
        return "ok" in self._do(f"param {name} {value}")

    def state(self):
        return next(l for l in self._do("state") if l.startswith("state ")).split()[1]

    def goals(self):
        return next(l for l in self._do("goals") if l.startswith("goals")).split()[1:]

    def take_cues(self):
        out, self.cues = self.cues, []
        return out


def chan(color):
    """A rope channel wearing `color` in the default palette."""
    return 1 + COLORS.index(color)


def burst(E, ch, n=6):
    for _ in range(n):
        E.pull(ch)
        E.wait(0.05)


# ── Scenarios: each returns what it saw ─────────────────────────────────────────
def mini(states, **top):
    cfg = {"game": "t", "start": next(iter(states)),
           "components": {"major": ["red", "blue", "green", "yellow", "orange"]},
           "cues": {"game": ["game_reset_components"], "audio_once": ["audio_once_a", "audio_once_b"]},
           "states": states}
    cfg.update(top)
    return cfg


def sc_charge(E):
    seen = []
    for _ in range(5):
        E.pull(chan("red"))
        E.wait(0.25)
    seen.append(E.state())                       # five do not fill it
    E.pull(chan("red"))
    seen.append(E.state())                       # the sixth does
    E.wait(14.5)
    seen.append(E.state())
    E.wait(1.0)                                  # 15 s from full: dark
    seen.append(E.state())
    return seen

SC_CHARGE = (mini({
    "idle": {"on": {"nongoal_component_activated": {"goto": "lit"}}},
    "lit": {"on": {"component_deactivated": {"goto": "dark"}}},
    "dark": {},
}), sc_charge, ["idle", "lit", "lit", "dark"])


def sc_simon(E):
    seen = []
    goals = E.goals()
    seen.append(len(goals))
    other = next(c for c in spec.GAME1_COLORS if c not in goals)
    burst(E, chan(other))
    for c in goals:
        burst(E, chan(c))
    seen.append(E.state())                       # a non-goal is lit: not yet
    E.wait(16)
    for c in goals:
        burst(E, chan(c))
    seen.append(E.state())
    return seen

SC_SIMON = (mini({
    "round": {"on": {"enter": {"goals": 2}, "all_components_activated": {"goto": "won"}}},
    "won": {},
}), sc_simon, [2, "round", "won"])


def sc_pull_first(E):
    E.pull(chan("red"))                          # starts the round; its charge lands after the reset
    seen = [E.state()]
    burst(E, chan("red"), 5)
    seen.append(E.state())
    return seen

SC_PULL_FIRST = (mini({
    "idle": {"on": {"pull": {"goto": "round"}}},
    "round": {"on": {"enter": {"cue": ["game_reset_components"]},
                     "nongoal_component_activated": {"goto": "lit"}}},
    "lit": {},
}), sc_pull_first, ["round", "lit"])


def sc_counters(E):
    E.take_cues()
    seen = []
    for _ in range(5):
        E.pull(chan("red"))
        E.wait(0.1)
        seen.append(E.take_cues())
    return seen

SC_COUNTERS = (mini(
    {"play": {"on": {"pull": {"add": {"hits": 1}}, "hits_reached": {"cue": ["audio_once_a"]}}}},
    counters={"hits": {"reach": "$need"}}, params={"need": 3},
), sc_counters, [[], [], ["audio_once_a"], [], []])


def sc_reach_beats_goto(E):
    E.take_cues()
    E.pull(chan("red"))
    return [E.state(), E.take_cues()]

SC_REACH = (mini({
    "s": {"on": {"pull": {"add": {"n": 1}, "cue": ["audio_once_a"], "goto": "t"},
                 "n_reached": {"goto": "won"}}},
    "t": {}, "won": {},
}, counters={"n": {"reach": 1}}), sc_reach_beats_goto, ["won", ["audio_once_a"]])


def sc_every_and_idle(E):
    E.take_cues()
    seen = []
    for _ in range(6):
        E.wait(1.0)
        E.pull(chan("red"))          # keeps idle from firing, does nothing to every
    seen.append(E.take_cues())
    E.wait(10.5)
    seen.append(E.take_cues().count("audio_once_b"))
    return seen

SC_EVERY = (mini({
    "s": {"every": {"delay": "1.5s", "cue": ["audio_once_a"]},
          "idle": {"delay": "10s", "cue": ["audio_once_b"]}},
}), sc_every_and_idle, [["audio_once_a"] * 4, 1])


def sc_params(E):
    seen = [E.param("wait", 99), E.param("fixed", 5)]
    E.wait(10.1)                     # the running timer kept its 10 s
    seen.append(E.state())
    E.wait(1.0 + 19.8)
    seen.append(E.state())           # back in a, now on the clamped 20 s
    E.wait(0.4)
    seen.append(E.state())
    return seen

SC_PARAMS = (mini({
    "a": {"after": {"delay": "$wait", "goto": "b"}},
    "b": {"after": {"delay": "1s", "goto": "a"}},
}, params={"wait": {"value": "10s", "min": "5s", "max": "20s"}, "fixed": 3}),
    sc_params, [True, False, "b", "a", "b"])


def sc_production(E):
    """The installation's game, start to finish and round again."""
    seen = []
    burst(E, chan("red"))
    seen.append(E.state())
    for _ in range(3):                           # easy, medium, hard
        for c in E.goals():
            burst(E, chan(c))
        seen.append(E.state())
    E.wait(61)                                   # the party, then the final
    seen.append(E.state())
    for ch in (6, 12, 18):                       # every purple rope with a rope on it
        burst(E, ch)
    seen.append(E.state())
    E.wait(121 + 121)                            # victory, kickout
    seen.append(E.state())
    burst(E, chan("red"))                        # all five were goals: it must still start
    seen.append(E.state())
    return seen

SC_PRODUCTION = (None, sc_production,
                 ["easy_round", "medium_round", "hard_round", "party", "final", "victory2",
                  "passive", "easy_round"])

SCENARIOS = {
    "charge": SC_CHARGE, "simon": SC_SIMON, "pull_first": SC_PULL_FIRST,
    "counters": SC_COUNTERS, "reach_beats_goto": SC_REACH, "every_and_idle": SC_EVERY,
    "params": SC_PARAMS, "production": SC_PRODUCTION,
}


@unittest.skipUnless(compiler() and os.path.isdir(JSON_LIB),
                     "needs a C++ compiler and ArduinoJson (pio run -e arbiter_mesh)")
class Conformance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="engine-host-")
        cls.binary = os.path.join(cls.tmp, "engine_host")
        host = os.path.join(ROOT, "playtest", "host")
        cmd = [compiler(), "-std=c++17", "-O1",
               "-I", os.path.join(host, "shim"), "-I", os.path.join(ROOT, "src", "arbiter_mesh"),
               "-I", JSON_LIB,
               "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
               "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
               os.path.join(ROOT, "src", "arbiter_mesh", "gamestate.cpp"),
               os.path.join(ROOT, "src", "arbiter_mesh", "components.cpp"),
               os.path.join(host, "stubs.cpp"), os.path.join(host, "driver.cpp"),
               "-o", cls.binary]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError("engine_host did not build:\n" + build.stderr[-3000:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def play(self, name):
        cfg, scenario, expected = SCENARIOS[name]
        if cfg is None:
            cfg = spec.load(os.path.join(ROOT, "game_state_machine", "state_config.yaml"))
        errors = game_to_json.validate(cfg)
        self.assertEqual(errors, [], name)
        path = os.path.join(self.tmp, f"{name}.json")
        with open(path, "w") as f:
            json.dump(game_to_json.for_arbiter(cfg), f)
        results = {}
        for E in (Python(), Cpp(self.binary)):
            E.load(cfg, path)
            try:
                results[E.name] = scenario(E)
            finally:
                if isinstance(E, Cpp):
                    E.close()
        self.assertEqual(results["python"], expected, f"{name}: python")
        self.assertEqual(results["c++"], expected, f"{name}: c++")


for _name in SCENARIOS:
    setattr(Conformance, f"test_{_name}", lambda self, n=_name: self.play(n))


if __name__ == "__main__":
    unittest.main()
