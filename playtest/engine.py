"""The game, headless, on a clock you hand it.

The arbiter runs the game in the installation (src/arbiter_mesh/gamestate.cpp
and components.cpp). This is the same machine in Python, so a simulated crowd
can play an hour of it in a fraction of a second and a night's play log can be
replayed through it. Overlord's --game mode runs it too, against the wall clock
(overlord/game.py). Keep it in step with the C++: test/test_engine.py holds the
behaviours both must share.

Driving it:

    eng = Engine(spec.load("game_state_machine/state_config.yaml"), seed=1)
    eng.on_record(print)          # every state, cue, pull, goal change ...
    eng.start()
    eng.pull(3)                   # a pull on rope channel 3, now
    eng.advance(2.5)              # let 2.5 s pass: timers fire, charge drains

Time is seconds since start(). Nothing happens between calls: advance() runs
every timer and every wear-out that falls due, in order, at its own instant.

Records are dicts with `t` and `kind`, the same shape overlord's play log
writes from the real installation (overlord/playlog.py), so one analysis reads
both:

    start   game, params, palette        state   name, prev
    cue     name, enter                  event   name, reacted
    pull    ch, color, cls (goal | nongoal | final)
    goals   colors                       palette colors (32 colour names)
    count   name, value                  param   name, value
    set     key, value                   input   name (a device's: diorama.door_opened)
"""

import itertools
import random

from . import spec
from .spec import (COLORS, PURPLE, CHANNEL_COUNT, MAJOR_COUNT, REACHED,
                   reactions, resolve_duration, resolve_number)

EPS = 1e-9

# Tie order when two things fall due at the same instant: the arbiter's loop
# runs gamestate_tick() before components_tick().
_PRIO = {"after": 0, "idle": 1, "at": 2, "every": 3, "wear": 4}


class _Comp:
    __slots__ = ("charge", "stamp", "on")

    def __init__(self):
        self.charge = 0.0
        self.stamp = 0.0
        self.on = False


class Engine:
    def __init__(self, cfg, *, seed=None, palette=None, roped=None, params=None):
        """cfg: a loaded game (spec.load). palette: channel -> colour index, 32
        long (default: the uncoloured install). roped: the channels with a rope
        on them; only those count toward the finals' win (default 1-20)."""
        self.cfg = cfg
        self.name = cfg.get("game", "game")
        self.states = cfg["states"]
        self.rng = random.Random(seed)
        self.params = spec.load_params(cfg)
        for k, v in (params or {}).items():
            self.set_param(k, v, quiet=True, force=True)   # a what-if may move a constant
        self.palette = list(palette or spec.default_palette())
        self.stored_palette = list(self.palette)
        self.roped = set(range(1, 21) if roped is None else roped)
        majors = cfg["components"]["major"]
        self.major_color = [spec.color_index(c) for c in majors]
        self.major_of_color = {c: i for i, c in enumerate(self.major_color)}
        self.majors = [_Comp() for _ in range(MAJOR_COUNT)]
        self.finals = [_Comp() for _ in range(CHANNEL_COUNT)]
        self.goal_mask = 0
        self.reach = spec.counters_of(cfg)
        self.counters = {name: 0 for name in self.reach}
        self.settings = {}
        for dev, body in (cfg.get("devices") or {}).items():
            for prop, values in ((body or {}).get("set") or {}).items():
                self.settings[f"{dev}.{prop}"] = values[0]
        self.goal_states = spec.goal_states(cfg)
        self.state = None
        self.t = 0.0
        self.entered_at = 0.0
        self.enters = 0
        self._after = None      # (due, goto)
        self._idle = None       # (due, period)
        self._every = []        # [[due, period, block]] one per `every` entry
        self._at = []           # [(due, key)] still to run in this state
        self._listeners = []
        self._seq = itertools.count()

    # -- outside ---------------------------------------------------------------
    def on_record(self, fn):
        """fn(record) for everything that happens, as it happens."""
        self._listeners.append(fn)

    @property
    def decay_rate(self):
        return float(resolve_number(self.cfg.get("default_decay_rate", spec.DEFAULT_DECAY_RATE),
                                    self.params))

    @property
    def charge_per_pull(self):
        return float(resolve_number(self.cfg.get("charge_per_pull", spec.DEFAULT_CHARGE_PER_PULL),
                                    self.params))

    @property
    def final_charge_per_pull(self):
        """A purple rope's pull; charge_per_pull when the game gives none."""
        value = self.cfg.get("final_charge_per_pull")
        return self.charge_per_pull if value is None else float(resolve_number(value, self.params))

    def start(self):
        self._emit("start", game=self.name,
                   params={k: p.value for k, p in self.params.items()},
                   palette=[COLORS[c] for c in self.palette])
        self._enter(self.cfg["start"])

    def advance(self, dt):
        self.advance_to(self.t + dt)

    def advance_to(self, t_end):
        """Let time run to t_end, firing everything that falls due on the way."""
        while True:
            due = self._next_due()
            if due is None or due[0] > t_end + EPS:
                break
            self.t = max(self.t, due[0])
            self._run_due(due)
        self.t = max(self.t, t_end)

    def next_due(self):
        """When something will next happen on its own, or None."""
        due = self._next_due()
        return due[0] if due else None

    def pull(self, ch):
        """A pull on rope channel `ch`, now. The pull events go first, so a
        state can react before the pull's charge goes in; then the charge, and
        any activation it causes."""
        if ch < 0 or ch >= CHANNEL_COUNT or self.state is None:
            return
        cls = self.classify(ch)
        if cls is None:
            return
        color = COLORS[self.palette[ch]]
        self._emit("pull", ch=ch, color=color, cls=cls)
        self._deliver("pull")
        self._deliver(cls + "_pull")
        if cls == "nongoal":
            self._deliver(f"nongoal_{color}_pull")

        # A reaction may have recoloured the ropes or re-dealt the goals:
        # the charge goes where things stand now.
        if self.palette[ch] == PURPLE:
            if not self._pour(self.finals[ch], self.final_charge_per_pull):
                return
            self._deliver("final_component_activated")
            if self._all_finals_lit():
                self._deliver("all_final_components_activated")
            return
        idx = self.major_of_color.get(self.palette[ch])
        if idx is None or not self._pour(self.majors[idx], self.charge_per_pull):
            return
        bit = 1 << idx
        if self.goal_mask & bit:
            if (self._lit_mask() & self.goal_mask) == bit:
                self._deliver("first_goal_component_activated")
            self._deliver("goal_component_activated")
        else:
            self._deliver("nongoal_component_activated")
        self._check_simon()

    def input(self, name):
        """An event by name: a device's input (diorama.door_opened), or any
        game event injected by hand. A device's input is also recorded as an
        `input`, as the arbiter reports it to the play log."""
        if name == "enter":
            return
        if "." in name:
            self._emit("input", name=name)
        self._deliver(name)

    def set_param(self, name, value, quiet=False, force=False):
        """Change a param live, clamped to its range. It applies the next time
        it is read: a running `after` keeps the deadline it was given. Only a
        param with a range may change live, as on the arbiter; `force` lets a
        simulation move a constant too."""
        p = self.params.get(name)
        if p is None:
            raise KeyError(f"no param {name!r}")
        if p.lo is None and not force:
            raise KeyError(f"param {name!r} is a constant (it has no min and max)")
        p.value = p.clamp(value) if p.lo is not None else value
        if not quiet:
            self._emit("param", name=name, value=p.value)
        return p.value

    # -- what a visitor could see ---------------------------------------------
    def classify(self, ch):
        color = self.palette[ch]
        if color == PURPLE:
            return "final"
        idx = self.major_of_color.get(color)
        if idx is None:
            return None
        return "goal" if self.goal_mask & (1 << idx) else "nongoal"

    def charge_of_major(self, idx):
        """0-100, as the display shows it (rounded up while it is lit)."""
        return self._charge_now(self.majors[idx])

    def charge_of_channel(self, ch):
        if self.palette[ch] == PURPLE:
            return self._charge_now(self.finals[ch])
        idx = self.major_of_color.get(self.palette[ch])
        return 0.0 if idx is None else self._charge_now(self.majors[idx])

    def lit_channel(self, ch):
        if self.palette[ch] == PURPLE:
            return self.finals[ch].on
        idx = self.major_of_color.get(self.palette[ch])
        return idx is not None and self.majors[idx].on

    def goals_visible(self):
        """Whether the room shows the goals now (spec.goal_states)."""
        return self.state in self.goal_states

    def goal_marks(self):
        """The goal mask the arbiter sends the displays: 0 outside a round."""
        return self.goal_mask if self.goals_visible() else 0

    @property
    def after_due(self):
        """When the state's `after` fires, in engine seconds, or None."""
        return self._after[0] if self._after else None

    def deal_goals(self, n):
        """The `goals: n` action, by hand."""
        self._randomize_goals(n)
        self._check_simon()

    def react(self, block):
        """Run one reaction block by hand (set, clear, add, colors, goals, cue)."""
        enters = self.enters
        self._act(block)
        if block.get("goto") and self.enters == enters:
            self._enter(block["goto"])

    def goal_colors(self):
        return [COLORS[self.major_color[i]] for i in range(MAJOR_COUNT) if self.goal_mask & (1 << i)]

    def time_in_state(self):
        return self.t - self.entered_at

    # -- the machine ------------------------------------------------------------
    def _emit(self, kind, **fields):
        rec = {"t": round(self.t, 3), "kind": kind, **fields}
        for fn in self._listeners:
            fn(rec)

    def _enter(self, name):
        prev = self.state
        self.state = name
        self.entered_at = self.t
        self.enters += 1
        self._after = self._idle = None
        self._every = []
        st = self.states[name] or {}
        self._emit("state", name=name, prev=prev)
        after = st.get("after")
        if after:
            delay = resolve_duration(after["delay"], self.params) if "delay" in after else spec.DEFAULT_AFTER
            self._after = (self.t + delay, after["goto"])
        if st.get("idle"):
            period = resolve_duration(st["idle"]["delay"], self.params)
            self._idle = (self.t + period, period)
        for block in spec.every_blocks(st):
            period = resolve_duration(block["delay"], self.params)
            self._every.append([self.t + period, period, block])
        self._at = sorted((self.t + resolve_duration(k, self.params), next(self._seq), k)
                          for k in (st.get("at") or {}))
        # Timers first: an entry cue can finish Simon's goal set and move the
        # machine on again before this returns, as on the arbiter.
        self._act(reactions(st).get("enter"), enter=True)

    def _deliver(self, name):
        if self.state is None:
            return
        if self._idle:
            self._idle = (self.t + self._idle[1], self._idle[1])   # any event is activity
        on = reactions(self.states[self.state])
        if name not in on or name == "enter":
            if name not in spec.PULL_EVENTS:
                self._emit("event", name=name, reacted=False)
            return
        self._emit("event", name=name, reacted=True)
        action = on[name] or {}
        enters = self.enters
        self._act(action)
        # A reaction that already moved the machine (a counter reaching its
        # mark, Simon completing) wins over its own goto.
        if action.get("goto") and self.enters == enters:
            self._enter(action["goto"])

    def _act(self, block, enter=False):
        """One reaction: set, clear, add, colors, goals, cue -- then any
        counter it brought to its mark. The caller handles goto."""
        block = block or {}
        for key, value in (block.get("set") or {}).items():
            if self.settings.get(key) != value:
                self.settings[key] = value
                self._emit("set", key=key, value=value)
        reached = []
        for name in block.get("clear") or []:
            self._count(name, 0, reached)
        for name, amount in (block.get("add") or {}).items():
            self._count(name, self.counters[name] + int(resolve_number(amount, self.params)), reached)
        colors = block.get("colors")
        if colors == "shuffle":
            self._shuffle_colors()
        elif colors == "restore":
            self._set_palette(self.stored_palette)
        dealt = False
        if block.get("goals") is not None:
            self._randomize_goals(int(resolve_number(block["goals"], self.params)))
            dealt = True
        dealt = self._fire(block.get("cue"), enter) or dealt
        if dealt:
            self._check_simon()   # a lit set can already match the new goals
        for name in reached:
            self._deliver(name)

    def _fire(self, cues, enter):
        dealt = False
        for cue in cues or []:
            if isinstance(cue, dict):   # {choose_one: [...]}: one of them, at random
                options = cue["choose_one"]
                cue = options[self.rng.randrange(len(options))]
            dealt = self._game_cue(cue) or dealt
            self._emit("cue", name=cue, enter=enter)
        return dealt

    def _game_cue(self, cue):
        """The `game_` cues the components act on. True if it dealt goals."""
        if cue == "game_reset_components":
            for c in self.majors + self.finals:
                c.charge, c.stamp, c.on = 0.0, self.t, False
            return False
        prefix, suffix = "game_randomize_simon_", "_goals"
        if cue.startswith(prefix) and cue.endswith(suffix):
            digits = cue[len(prefix):-len(suffix)]
            if digits.isdigit() and 1 <= int(digits) <= MAJOR_COUNT:
                self._randomize_goals(int(digits))
                return True
        return False

    def _count(self, name, value, reached):
        value = max(0, value)   # a count of misses cannot go below none
        old = self.counters[name]
        if value == old:
            return
        self.counters[name] = value
        self._emit("count", name=name, value=value)
        mark = self.reach.get(name)
        if mark is not None:
            mark = resolve_number(mark, self.params)
            if old < mark <= value:
                reached.append(name + REACHED)

    def _randomize_goals(self, n):
        n = max(1, min(MAJOR_COUNT, n))
        choices = [m for m in range(1, 1 << MAJOR_COUNT) if bin(m).count("1") == n]
        fresh = [m for m in choices if m != self.goal_mask]
        pool = fresh or choices
        self.goal_mask = pool[self.rng.randrange(len(pool))]
        self._emit("goals", colors=self.goal_colors())

    def _shuffle_colors(self):
        """Permute game1's colours among the ropes, purple staying put
        (installcfg_shuffle_game1), drawn until something actually moves.
        Only for the game: the stored palette is what `restore` returns to."""
        n = spec.MAJOR_COUNT
        while True:
            perm = list(range(n))
            for i in range(n - 1, 0, -1):
                j = self.rng.randrange(i + 1)
                perm[i], perm[j] = perm[j], perm[i]
            if any(perm[i] != i for i in range(n)):
                break
        self._set_palette([perm[c] if c < n else c for c in self.palette])

    def _set_palette(self, palette):
        if list(palette) == self.palette:
            return
        self.palette = list(palette)
        self._emit("palette", colors=[COLORS[c] for c in self.palette])

    # -- components (components.cpp) -------------------------------------------
    def _settle(self, c):
        if c.charge > 0.0:
            c.charge -= self.decay_rate * (self.t - c.stamp)
            if c.charge < EPS:
                c.charge = 0.0
        c.stamp = self.t

    def _charge_now(self, c):
        return max(0.0, c.charge - self.decay_rate * (self.t - c.stamp))

    def _pour(self, c, amount):
        """One pull's charge. True if that filled a dark component."""
        self._settle(c)
        c.charge = min(100.0, c.charge + amount)
        if not c.on and c.charge >= 100.0:
            c.on = True
            return True
        return False

    def _lit_mask(self):
        return sum(1 << i for i, c in enumerate(self.majors) if c.on)

    def _all_finals_lit(self):
        roped = [ch for ch in self.roped if self.palette[ch] == PURPLE]
        return bool(roped) and all(self.finals[ch].on for ch in roped)

    def _holds(self, condition):
        """An `every` entry's `if:` (spec.EVERY_CONDITIONS), as things stand now."""
        if not condition:
            return True
        goals = [self.majors[i] for i in range(MAJOR_COUNT) if self.goal_mask & (1 << i)]
        if condition == "goal_active":
            return any(c.on for c in goals)
        if condition == "no_goal_charge":
            return bool(goals) and all(self._charge_now(c) <= EPS for c in goals)
        return False

    def _check_simon(self):
        if self.goal_mask and self._lit_mask() == self.goal_mask:
            self._deliver("all_components_activated")

    # -- timers -----------------------------------------------------------------
    def _next_due(self):
        best = None

        def offer(t, kind, data=None):
            nonlocal best
            key = (t, _PRIO[kind])
            if best is None or key < (best[0], _PRIO[best[1]]):
                best = (t, kind, data)

        if self._after:
            offer(self._after[0], "after")
        if self._idle:
            offer(self._idle[0], "idle")
        if self._at:
            offer(self._at[0][0], "at")
        for i, entry in enumerate(self._every):
            offer(entry[0], "every", i)
        rate = self.decay_rate
        for i, c in enumerate(self.majors):
            if c.on:
                offer(c.stamp + c.charge / rate, "wear", ("major", i))
        for ch, c in enumerate(self.finals):
            if c.on:
                offer(c.stamp + c.charge / rate, "wear", ("final", ch))
        return best

    def _run_due(self, due):
        _, kind, data = due
        st = self.states[self.state] or {}
        if kind == "after":
            goto = self._after[1]
            self._after = None
            self._emit("timer", name="after")
            self._enter(goto)
        elif kind == "idle":
            self._idle = (self.t + self._idle[1], self._idle[1])   # before: a cue may move us
            self._emit("timer", name="idle")
            self._act(st.get("idle"))
        elif kind == "every":
            entry = self._every[data]
            entry[0] = self.t + entry[1]
            if self._holds(entry[2].get("if")):
                self._emit("timer", name="every")
                self._act(entry[2])
        elif kind == "at":
            _, _, key = self._at.pop(0)
            self._emit("timer", name=f"at {key}")
            self._act((st.get("at") or {})[key])
        elif kind == "wear":
            which, i = data
            c = self.majors[i] if which == "major" else self.finals[i]
            self._settle(c)
            c.charge, c.on = 0.0, False
            if which == "final":
                self._deliver("final_component_deactivated")
                return
            self._deliver("component_deactivated")
            if not any(m.on for m in self.majors):
                self._deliver("all_components_deactivated")
            self._check_simon()
