"""The room: the engine, the ropes, the screen, and the visitors in it.

One World runs a game against simulated people on a virtual clock. Two shapes:

    episode   one group walks up to an idle installation, plays, leaves; the
              run ends once they have gone and the game has settled. Cheap and
              comparable, so the tuner uses it.
    night     groups arrive at random for hours and overlap, as at the event.
              Its play log looks like a real night's (playtest/metrics.py
              reads both the same way).

What the room shows people, and how faithfully:

  * Clips: a video_once fired for an event is dropped while another clip is
    playing, as overlord does; one fired on entering a state cuts in. So
    praise can be lost when activations come close together -- and a visitor
    who misses it learns the wrong lesson.
  * The component display shows each colour's charge; a visitor sees their own
    rope's bar rise and fill.
  * Goals are shown while a round is on: each extractor outlines its tank when
    its colour is wanted (goal_marks), and overlord draws them over the video
    with the game's `show_goals`. goal_marks=False is the room before either.
  * A rope's sensor counts one pull, then cools down (SENSOR_COOLDOWN): yanking
    faster gets no more. A rough yank trips its guard -- nothing counts and the
    handle stays dark until the rope has been calm for ROUGH_CALM seconds.
"""

import random

from .engine import Engine
from .players import Population, Visitor, meaning_of_cue, meaning_of_setting
from .spec import COLORS, PURPLE

CLIP_SECONDS = 4.0   # a typical Janis clip


class World:
    def __init__(self, cfg, *, seed=0, params=None, roped=None, palette=None,
                 population=None, show_goals=None, goal_marks=True, clip_seconds=CLIP_SECONDS):
        self.rng = random.Random(seed)
        self.engine = Engine(cfg, seed=self.rng.randrange(1 << 30), params=params,
                             roped=roped, palette=palette)
        self.population = population or Population()
        self.show_goals = bool(cfg.get("show_goals", False) if show_goals is None else show_goals)
        self.goals_on_display = self.show_goals or goal_marks
        self._counted = {}            # channel -> when its sensor last counted a pull
        self._rough_until = {}        # channel -> when its guard calms down
        self.clip_seconds = clip_seconds
        self.clip_until = -1.0
        self.trace = []
        self.visitors = []
        self.groups = {}
        self.occupied = {}            # channel -> visitor id
        self._reported = set()        # groups whose leaving is in the trace
        self._flash_until = -1.0
        self.here = []                # visitors still in the room
        self._open = set()            # groups not all gone yet

        self._next_id = 0
        self.engine.on_record(self._on_record)

    @property
    def t(self):
        return self.engine.t

    # -- what visitors can ask of the room ----------------------------------------
    def color_of(self, ch):
        return self.engine.palette[ch]

    def lit(self, ch):
        return self.engine.lit_channel(ch)

    def charge(self, ch):
        return self.engine.charge_of_channel(ch)

    SENSOR_COOLDOWN = 1.7   # a pull's bout, then pulleys_detect's 1.5 s refractory
    ROUGH_CALM = 2.5        # RoughConfig.calmMs

    def pull(self, visitor, rough=False):
        """A yank on visitor's rope, through its sensor. True if it counted."""
        ch, t = visitor.rope, self.t
        if rough:
            self._rough_until[ch] = t + self.ROUGH_CALM
            return False
        if t < self._rough_until.get(ch, -1.0) or t - self._counted.get(ch, -9.0) < self.SENSOR_COOLDOWN:
            return False
        self._counted[ch] = t
        self.engine.pull(ch)
        return True

    def release(self, ch):
        self.occupied.pop(ch, None)

    def pick_rope(self, v):
        """A free rope for v: the colour they most believe in, with a curious
        visitor sometimes trying something else."""
        free = [ch for ch in sorted(self.engine.roped) if ch not in self.occupied or self.occupied[ch] == v.id]
        if not free:
            return None
        def score(ch):
            c = self.engine.palette[ch]
            want = v.belief[c]
            if v.goals_shown is not None and COLORS[c] in v.goals_shown:
                want = 0.95
            return want + v.tr.curiosity * self.rng.random() * 0.6
        best = max(free, key=score)
        if v.rope is not None:
            self.occupied.pop(v.rope, None)
        self.occupied[best] = v.id
        return best

    def rope_needing(self, v):
        """A free rope of a colour v wants that is dark and that nobody present
        is pulling, or None. Each purple rope is a final of its own, so any
        dark one is needed, whoever is on the others."""
        manned = {self.engine.palette[ch] for ch, who in self.occupied.items() if who != v.id}
        best = None
        for ch in sorted(self.engine.roped):
            if ch in self.occupied:
                continue
            c = self.engine.palette[ch]
            if (c in manned and c != PURPLE) or self.lit(ch) or v.want_color(c) <= 0.6:
                continue
            if best is None or v.want_color(c) > v.want_color(self.engine.palette[best]):
                best = ch
        return best

    def visible_goals(self):
        """The goals the room is showing now: only while the state cares about
        goals, as the arbiter and overlord decide it (spec.goal_states)."""
        if not self.goals_on_display or not self.engine.goals_visible():
            return None
        return set(self.engine.goal_colors()) or None

    # -- the engine's records become what people perceive -------------------------
    def _on_record(self, rec):
        self.trace.append(rec)
        p = self._percept(rec)
        if p is None:
            return
        for v in self.here:
            if v.left is None:
                v.perceive(p, self)
        if rec["kind"] == "set" and rec["key"] == "diorama.latch" and rec["value"] == "open":
            self._diorama_opens()

    def _diorama_opens(self):
        """In the first victory the diorama unlatches. Inside: the art spirits,
        their souls drawn up through tubes for Janis, and signage hinting that
        purple sets them free. Whoever reads it may tell their group."""
        went_in = False
        for v in list(self.here):
            if v.left is None and v.look_in_diorama(self):
                went_in = True
        for v in list(self.here):
            if v.left is None and v.knows_twist:
                for other in self.groups[v.group][1]:
                    if other.left is None and not other.knows_twist and v.rng.random() < other.tr.social:
                        other.learn_twist(self.t, 0.8)
        if went_in:
            self.engine.input("diorama.door_opened")   # as the door switch would report

    FLASH_SECONDS = 1.8   # the diorama's purple flash; a press during it is not another press

    def press_button(self, v):
        """"Free the spirits", pressed. The board flashes purple whatever the
        game is doing; the game hears diorama.button_pressed and answers with
        a bad noise and Janis."""
        if self.t < self._flash_until:
            return
        self._flash_until = self.t + self.FLASH_SECONDS
        v.pressed(self)
        self.engine.input("diorama.button_pressed")

    def _percept(self, rec):
        kind = rec["kind"]
        if kind == "cue":
            name = rec["name"]
            m = meaning_of_cue(name)
            if m is None:
                return None
            bare = name.split("__", 1)[-1]
            if bare.startswith("video_once_"):
                if self.t < self.clip_until and not rec.get("enter"):
                    rec["dropped"] = True          # overlord would skip it
                    return None
                self.clip_until = self.t + self.clip_seconds
            return {"kind": "cue", "meaning": m[0], "family": m[1]}
        if kind == "set":
            meaning = meaning_of_setting(rec["key"], rec["value"])
            return meaning and {"kind": "setting", "meaning": meaning, "key": rec["key"], "value": rec["value"]}
        if kind == "palette":
            return {"kind": "palette"}
        if kind in ("goals", "state") and self.visible_goals() is not None:
            # new goals dealt, or a round begun: the outlines change
            return {"kind": "goals", "colors": sorted(self.visible_goals())}
        return None

    # -- people come and go ----------------------------------------------------------
    def arrive(self, kind=None, traits=None):
        """A group walks in. Returns the group id."""
        if traits is None:
            kind, traits = self.population.group(self.rng)
        gid = len(self.groups)
        members = []
        for tr in traits:
            v = Visitor(self._next_id, gid, tr, random.Random(self.rng.randrange(1 << 30)), self.t)
            v.goals_shown = self.visible_goals()
            self._next_id += 1
            self.visitors.append(v)
            self.here.append(v)
            members.append(v)
        self.groups[gid] = (kind, members)
        self._open.add(gid)
        self._truth("arrive", group=gid, archetype=kind, size=len(members))
        return gid

    def _truth(self, what, **fields):
        # Ground truth only the simulation knows. Analysis of a real log never sees these.
        self.trace.append({"t": round(self.t, 3), "kind": "truth", "what": what, **fields})

    def present(self):
        return [v for v in self.here if v.left is None]

    def step(self):
        dt = Visitor.TICK
        self.engine.advance(dt)
        for v in list(self.here):
            if v.left is None:
                v.tick(self)
                if v.left is not None and v.rope is not None:
                    self.release(v.rope)
        # A group goes together once most of it wants to.
        for gid in sorted(self._open):
            kind, members = self.groups[gid]
            gone = sum(1 for v in members if v.left is not None)
            if 0 < gone < len(members) and gone * 2 >= len(members):
                for v in members:
                    if v.left is None:
                        v.leave(self.t, "with_group")
                        if v.rope is not None:
                            self.release(v.rope)
                gone = len(members)
            if gone == len(members) and gid not in self._reported:
                self._reported.add(gid)
                self._open.discard(gid)
                self._truth("leave", group=gid, archetype=kind,
                            fun=round(sum(v.fun for v in members) / len(members), 3),
                            reasons=sorted({v.leave_reason for v in members}),
                            pulls=sum(v.pulls for v in members))
        self.here = [v for v in self.here if v.left is None]

    # -- runs ---------------------------------------------------------------------------
    def run_episode(self, kind=None, traits=None, settle=60.0, cap=1800.0):
        """One group plays an idle installation. Returns the group's outcome."""
        self.engine.start()
        self.engine.advance(1.0)
        gid = self.arrive(kind, traits)
        members = self.groups[gid][1]
        while self.t < cap and any(v.left is None for v in members):
            self.step()
        # let the game run out its timers so the log shows where it would end up
        end = self.t + settle
        while self.t < end:
            self.engine.advance(Visitor.TICK)
        return outcome(self.groups[gid])

    def run_night(self, hours=3.0, groups_per_hour=20.0, each_second=None):
        """Groups arrive at random for `hours`. each_second() is called once a
        simulated second, for whatever watches the night as it goes (the live
        director)."""
        self.engine.start()
        end = hours * 3600.0
        next_arrival = self.rng.expovariate(groups_per_hour / 3600.0)
        next_tick = 0.0
        while self.t < end:
            if self.t >= next_arrival:
                self.arrive()
                next_arrival = self.t + self.rng.expovariate(groups_per_hour / 3600.0)
            if self.present():
                self.step()
            else:
                # nobody in: jump to whichever comes first, the next group or a timer
                nxt = self.engine.next_due()
                target = min(next_arrival, end, nxt if nxt is not None else end,
                             next_tick if each_second else end)
                self.engine.advance_to(max(target, self.t + 0.01))
            if each_second and self.t >= next_tick:
                each_second()
                next_tick = self.t + 1.0
        return [outcome(g) for g in self.groups.values()]


def outcome(group):
    kind, members = group
    stay = [(v.left if v.left is not None else v.arrived) - v.arrived for v in members]
    return {
        "archetype": kind,
        "size": len(members),
        "fun": sum(v.fun for v in members) / len(members),
        "stay": sum(stay) / len(stay),
        "pulls": sum(v.pulls for v in members),
        "reasons": sorted({v.leave_reason or "still_here" for v in members}),
    }
