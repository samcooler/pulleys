"""What a game file may say, shared by the validator and the engine.

The arbiter's C++ (src/arbiter_mesh/gamestate.cpp) reads the same rules out of
the JSON that tools/game_to_json.py makes. Keep the three in step.

Params
    A game names its tunable values under `params:` and uses them as `$name`
    wherever a duration or a count goes. One with a `min` and a `max` is a
    knob the tuner may turn; one with only a value is a named constant.

        params:
          round_time:
            value: 2m
            min: 1m
            max: 3m
          nag_delay: 20s          # a constant: no range, never tuned

    Durations are stored as seconds, so `$round_time` in a delay is 120.
"""

import math
import re

import yaml

COLORS = ("red", "blue", "green", "yellow", "orange", "purple")
GAME1_COLORS = COLORS[:5]
PURPLE = 5
CHANNEL_COUNT = 32
MAJOR_COUNT = 5

DEFAULT_DECAY_RATE = 100.0 / 15.0
DEFAULT_CHARGE_PER_PULL = 20.0
MAX_DECAY_RATE = 65.0
DEFAULT_AFTER = 120.0

# Every event the arbiter itself sends. Keep in step with gameevents.h.
COMPONENT_EVENTS = (
    "first_goal_component_activated",
    "goal_component_activated",
    "nongoal_component_activated",
    "component_deactivated",
    "all_components_activated",
    "all_components_deactivated",
    "final_component_activated",
    "final_component_deactivated",
    "all_final_components_activated",
)
# One per pull, before the pull's charge goes in: `pull` for any rope, then
# the kind of rope it was.
PULL_EVENTS = ("pull", "goal_pull", "nongoal_pull", "final_pull")
KNOWN_EVENTS = frozenset(COMPONENT_EVENTS + PULL_EVENTS)

# A state that reacts to any of these cares about goals, and only then does the
# room show them: the extractors' outlines, overlord's band. Keep in step with
# GOAL_EVENTS in src/arbiter_mesh/gamestate.cpp.
GOAL_EVENTS = ("goal_pull", "nongoal_pull", "goal_component_activated",
               "first_goal_component_activated", "all_components_activated")


def goal_states(cfg):
    """The states that show the goals."""
    return {name for name, st in cfg["states"].items()
            if any(e in reactions(st or {}) for e in GOAL_EVENTS)}

# Limits the arbiter's fixed tables impose. Keep in step with gamestate.cpp.
PARAMS_MAX = 16
PARAM_NAME_MAX = 23
COUNTERS_MAX = 8
COUNTER_NAME_MAX = 15
REACHED = "_reached"

NAME = re.compile(r"[a-z][a-z0-9_]*")
DURATION = re.compile(r"\s*(\d+(?:\.\d+)?)\s*(s|m)\s*")

# The keys an action block may hold, in the order they run.
ACTION_KEYS = ("set", "clear", "add", "colors", "goals", "cue", "goto")
COLOR_ACTIONS = ("shuffle", "restore")


def fix_keys(node):
    """YAML 1.1 reads a bare `on:` key as boolean True (and `off:` as False);
    spell them back as the words they were written as."""
    if isinstance(node, dict):
        return {{True: "on", False: "off"}.get(k, k) if isinstance(k, bool) else k:
                fix_keys(v) for k, v in node.items()}
    if isinstance(node, list):
        return [fix_keys(v) for v in node]
    return node


def load(path):
    with open(path) as f:
        return fix_keys(yaml.safe_load(f))


def reactions(state):
    return (state or {}).get("on") or {}


def default_palette():
    """Channel -> colour index for an install nobody has recoloured
    (pulleys_channel default_channel_color)."""
    return [(ch + len(COLORS) - 1) % len(COLORS) for ch in range(CHANNEL_COUNT)]


def color_index(name):
    return COLORS.index(name)


def parse_duration(text):
    """`10s`, `1.5s` or `2m` -> seconds. ValueError otherwise."""
    m = DURATION.fullmatch(str(text))
    if not m or isinstance(text, bool):
        raise ValueError(f"bad duration {text!r}; use e.g. 10s, 1.5s or 2m")
    return float(m.group(1)) * (60 if m.group(2) == "m" else 1)


def is_ref(value):
    return isinstance(value, str) and value.startswith("$")


class Param:
    """One named value. `kind` is duration (seconds), int or number."""

    def __init__(self, name, value, kind, lo=None, hi=None, note="", tune=True):
        self.name, self.value, self.kind = name, value, kind
        self.lo, self.hi, self.note = lo, hi, note
        self.default = value
        self.tunable = tune and lo is not None and hi is not None and hi > lo

    def clamp(self, v):
        if self.lo is not None:
            v = max(self.lo, v)
        if self.hi is not None:
            v = min(self.hi, v)
        if self.kind == "int":
            v = int(round(v))
        return v

    def show(self, v=None):
        v = self.value if v is None else v
        if self.kind == "duration":
            return f"{v:g}s" if v < 120 or v % 60 else f"{v / 60:g}m"
        return f"{v:g}"

    def as_json(self):
        out = {"value": self.value}
        if self.lo is not None:
            out["min"] = self.lo
        if self.hi is not None:
            out["max"] = self.hi
        return out

    def __repr__(self):
        rng = f" [{self.show(self.lo)}..{self.show(self.hi)}]" if self.tunable else ""
        return f"<{self.name}={self.show()}{rng}>"


def _scalar(value, where):
    """(number, kind) for a param's value, min or max."""
    if isinstance(value, bool):
        raise ValueError(f"{where}: {value!r} is not a number or a duration")
    if isinstance(value, int):
        return value, "int"
    if isinstance(value, float):
        return value, "number"
    if isinstance(value, str):
        return parse_duration(value), "duration"
    raise ValueError(f"{where}: {value!r} is not a number or a duration")


def load_params(cfg, errors=None):
    """{name: Param} from cfg['params']. Problems go to `errors` if given,
    else raise ValueError."""
    out = {}
    problems = []
    body = cfg.get("params") or {}
    if not isinstance(body, dict):
        problems.append("params: must be a map of name: value")
        body = {}
    for name, decl in body.items():
        where = f"params.{name}"
        if not isinstance(name, str) or not NAME.fullmatch(name) or len(name) > PARAM_NAME_MAX:
            problems.append(f"{where}: a name is lowercase letters, digits and _, "
                            f"starting with a letter, at most {PARAM_NAME_MAX} characters")
            continue
        try:
            if isinstance(decl, dict):
                extra = set(decl) - {"value", "min", "max", "note", "tune"}
                if extra:
                    problems.append(f"{where}: unknown key {sorted(extra)}; use value, min, max, note, tune")
                if "value" not in decl:
                    problems.append(f"{where}: missing value")
                    continue
                value, kind = _scalar(decl["value"], f"{where}.value")
                lo = hi = None
                if "min" in decl:
                    lo, k = _scalar(decl["min"], f"{where}.min")
                    kind = _merge_kind(kind, k, where)
                if "max" in decl:
                    hi, k = _scalar(decl["max"], f"{where}.max")
                    kind = _merge_kind(kind, k, where)
                if (lo is None) != (hi is None):
                    problems.append(f"{where}: give both min and max, or neither")
                if lo is not None and hi is not None and not lo <= value <= hi:
                    problems.append(f"{where}: value must lie between min and max")
                out[name] = Param(name, value, kind, lo, hi, str(decl.get("note") or ""),
                                  decl.get("tune", True) is not False)
            else:
                value, kind = _scalar(decl, where)
                out[name] = Param(name, value, kind)
        except ValueError as e:
            problems.append(str(e) if str(e).startswith(where) else f"{where}: {e}")
    if len(out) > PARAMS_MAX:
        problems.append(f"params: {len(out)} of them, at most {PARAMS_MAX}")
    if problems:
        if errors is None:
            raise ValueError("; ".join(problems))
        errors.extend(problems)
    return out


def _merge_kind(a, b, where):
    if a == b:
        return a
    if {a, b} == {"int", "number"}:
        return "number"
    raise ValueError(f"{where}: value, min and max must all be durations or all be numbers")


def resolve_duration(value, params):
    """A delay: `10s`, or `$name` of a duration param. Seconds."""
    if is_ref(value):
        p = params[value[1:]]
        return float(p.value)
    return parse_duration(value)


def resolve_number(value, params):
    """A count or a rate: a number, or `$name` of a number param."""
    if is_ref(value):
        return params[value[1:]].value
    return value


def check_ref(value, params, want, where):
    """None if `value` is a fine `want` (duration, int, number), else a reason."""
    if is_ref(value):
        p = params.get(value[1:])
        if p is None:
            return f"{where}: {value} is not a param"
        if want == "duration" and p.kind != "duration":
            return f"{where}: {value} is a number; a delay needs a duration param (like 30s)"
        if want in ("int", "number") and p.kind == "duration":
            return f"{where}: {value} is a duration; this needs a number"
        if want == "int" and p.kind != "int":
            return f"{where}: {value} must be a whole number"
        return None
    if want == "duration":
        try:
            parse_duration(value)
            return None
        except ValueError:
            return f"{where}: must look like 10s, 1.5s or 2m, or name a param ($name)"
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return f"{where}: must be a number or name a param ($name)"
    if want == "int" and not isinstance(value, int):
        return f"{where}: must be a whole number"
    return None


def counters_of(cfg):
    """{name: reach-or-None} from cfg['counters']."""
    out = {}
    for name, decl in (cfg.get("counters") or {}).items():
        out[name] = (decl or {}).get("reach") if isinstance(decl, dict) else None
    return out


def params_for_json(params):
    """The params as the arbiter takes them: plain numbers, durations in seconds."""
    return {name: p.as_json() for name, p in params.items()}


def milli(value):
    """A value as the integer thousandths the arbiter's telemetry carries."""
    return int(math.floor(value * 1000 + 0.5))
