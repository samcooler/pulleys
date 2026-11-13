"""Simulated visitors: who comes in, what they notice, what they pull, when they leave.

A visitor stands at one rope and pulls it. What they can know is only what the
room shows them: the colour of the ropes, the bars on the component display
(their own colour's charge rising), the clips and sounds, the room's lights.
They do NOT see the game's state or, unless the game shows them, its goals.
So in a Simon round they have to work out which colours are wanted from Janis
praising some activations and ignoring others -- and the model learns exactly
that way: a praise shortly after their colour lit raises their belief that it
is wanted; a lit colour met with silence lowers it.

Each visitor carries three numbers over time:

    affect    how good this moment feels; bumped by what they perceive, fading
              back to 0 over ~10 s. Its integral is the visitor's *fun* -- the
              simulation's ground truth, which no sensor can see.
    interest  whether they stay. Fed by affect, worn down by boredom (which
              grows the longer nothing new happens). They leave at 0, or when
              their time budget runs out, or with their group.
    beliefs   per colour, how sure they are that pulling it is wanted.

Every coefficient here is a guess to be replaced by measurement:
playtest/calibrate.py fits the population to real play logs.
"""

import math
import re
from dataclasses import dataclass, field, replace

from .spec import COLORS, PURPLE

# ── What a cue means to someone standing in the room ─────────────────────────
# First match wins. The meaning sets how it feels (IMPACT) and what it teaches.
CUE_MEANINGS = [
    (r"denied", "rebuff"),
    (r"progress_confirmation|productivity_score|corporate_approval", "progress"),
    (r"congrats|celebration|pizza|victory|fanfare|liberation|party", "celebrate"),
    (r"encouragement", "praise"),
    (r"powerup", "gain"),
    (r"powerdown|distress|shutdown|defeat", "loss"),
    (r"nag|suspicion|idle_return", "nag"),
    (r"warn|beg|anger|threaten|panic|disciplin", "scold"),
    (r"revelation|cage|unlock|stinger|forced_labor", "reveal"),
]
_MEANINGS = [(re.compile(p), m) for p, m in CUE_MEANINGS]

# How it feels the first time, as a bump to affect. Scold is per visitor:
# compliant people dislike it, rebels enjoy it (see Traits.compliance).
# "rebuff" is the room answering something a visitor did with a big bad noise
# (the diorama's button): bad news in the story, and in the theatre a moment.
IMPACT = {
    "praise": 0.25, "progress": 0.45, "celebrate": 0.7, "gain": 0.06, "rebuff": 0.2,
    "loss": -0.12, "nag": -0.04, "reveal": 0.55, "shift": 0.18, "scene": 0.12,
    "lit": 0.12, "agency": 0.015,
}


def meaning_of_cue(name):
    """(meaning, family) for a cue name as a visitor experiences it, or None for
    something they would not notice as an event (a game control cue)."""
    if TARGET_SPLIT in name:
        name = name.split(TARGET_SPLIT, 1)[1]
    if name.startswith("game_") or name in ("audio_loop_off", "audio_bed_off"):
        return None
    if name.startswith(("video_loop_", "audio_loop_", "audio_bed_")):
        return "scene", name
    family = name.split("*")[0].rstrip("_")
    family = re.sub(r"_\d+$", "", family)
    for pattern, meaning in _MEANINGS:
        if pattern.search(name):
            return meaning, family
    return "ambient", family


TARGET_SPLIT = "__"


def meaning_of_setting(key, value):
    """A device changing, as the room sees it."""
    if value in ("dark", "locked", "off", "closed"):
        return None
    if key.startswith("party"):
        return "celebrate"
    return "reveal"


@dataclass
class Traits:
    """One visitor. Rates are per second, times in seconds."""
    archetype: str = "visitor"
    pull_rate: float = 1.2       # pulls a second when keen
    attention: float = 0.7       # chance of noticing any one thing
    curiosity: float = 0.4       # appetite for switching ropes, trying purple
    compliance: float = 0.6      # 1: does what Janis says; 0: does the opposite for fun
    learn: float = 0.4           # how far one lesson moves a belief
    interest0: float = 0.7       # interest on arrival
    boredom: float = 0.004       # interest lost per second when nothing is new
    boredom_ramp: float = 45.0   # seconds of sameness that double the boredom
    budget: float = 300.0        # the most time they have, whatever happens
    stamina: float = 250.0       # pulls before their arm slows them to half
    social: float = 0.5          # how much of the group's lessons they take on
    reaction: float = 0.8        # seconds to act on something they noticed
    rough: float = 0.05          # chance a yank is rough enough to trip the sensor's guard


# ── Who comes in ──────────────────────────────────────────────────────────────
# (weight, group size range, trait means). Each visitor's traits scatter
# around their archetype's (lognormal for rates and times).
ARCHETYPES = {
    "passerby": (0.35, (1, 2), Traits("passerby", rough=0.04, pull_rate=0.8, attention=0.5, curiosity=0.2,
                                      compliance=0.7, learn=0.3, interest0=0.45, boredom=0.006,
                                      budget=90, social=0.3)),
    "family":   (0.25, (2, 5), Traits("family", rough=0.12, pull_rate=1.5, attention=0.6, curiosity=0.5,
                                      compliance=0.6, learn=0.35, interest0=0.8, boredom=0.004,
                                      budget=420, social=0.7)),
    "solver":   (0.15, (1, 3), Traits("solver", rough=0.02, pull_rate=1.0, attention=0.9, curiosity=0.6,
                                      compliance=0.8, learn=0.6, interest0=0.9, boredom=0.0035,
                                      budget=600, social=0.6)),
    "rebel":    (0.10, (2, 4), Traits("rebel", rough=0.2, pull_rate=1.6, attention=0.7, curiosity=0.9,
                                      compliance=0.1, learn=0.4, interest0=0.8, boredom=0.005,
                                      budget=360, social=0.5)),
    "party":    (0.15, (4, 8), Traits("party", rough=0.25, pull_rate=2.0, attention=0.4, curiosity=0.5,
                                      compliance=0.4, learn=0.25, interest0=0.85, boredom=0.005,
                                      budget=300, social=0.8)),
}

_SPREAD = {"pull_rate": 0.3, "budget": 0.5, "boredom": 0.3, "stamina": 0.4, "reaction": 0.3}


@dataclass
class Population:
    """The mix of people who come through, and how they scatter.
    `scale` multiplies trait means by name (calibrate.py fits these)."""
    weights: dict = field(default_factory=lambda: {k: v[0] for k, v in ARCHETYPES.items()})
    scale: dict = field(default_factory=dict)

    def group(self, rng):
        names = list(self.weights)
        kind = rng.choices(names, [self.weights[n] for n in names])[0]
        _, (lo, hi), base = ARCHETYPES[kind]
        size = rng.randint(lo, hi)
        return kind, [self.traits(kind, rng) for _ in range(size)]

    def traits(self, kind, rng):
        base = ARCHETYPES[kind][2]
        vals = {}
        for name, sigma in _SPREAD.items():
            mean = getattr(base, name) * self.scale.get(name, 1.0)
            vals[name] = mean * math.exp(rng.gauss(0, sigma) - sigma * sigma / 2)
        for name in ("attention", "curiosity", "compliance", "learn", "interest0", "social", "rough"):
            mean = getattr(base, name) * self.scale.get(name, 1.0)
            spread = 0.05 if name == "rough" else 0.12
            vals[name] = min(1.0, max(0.0 if name == "rough" else 0.02, rng.gauss(mean, spread)))
        return replace(base, **vals)

    def as_json(self):
        return {"weights": self.weights, "scale": self.scale}

    @classmethod
    def from_json(cls, d):
        return cls(weights=dict(d.get("weights") or cls().weights), scale=dict(d.get("scale") or {}))


class Visitor:
    TICK = 0.25
    AFFECT_TAU = 10.0
    INTEREST_GAIN = 0.1      # interest per unit of affect-second
    CREDIT_WINDOW = 4.0      # a praise this soon after a colour lit is credited to it
    FORGET_TAU = 40.0        # beliefs drift back toward unsure

    def __init__(self, vid, group, traits, rng, t):
        self.id, self.group, self.tr, self.rng = vid, group, traits, rng
        self.arrived = t
        self.left = None
        self.leave_reason = None
        self.rope = None
        self.walk_until = t + rng.uniform(1.0, 4.0)   # finding a rope
        self.affect = 0.25                             # arriving is a small thrill
        self.interest = traits.interest0
        self.fun = 0.0
        self.pulls = 0
        self.belief = [0.5] * len(COLORS)
        self.belief[PURPLE] = 0.5 + 0.3 * (0.5 - traits.compliance)
        self.seen = {}            # cue family -> times perceived
        self.seen_meaning = {}
        self.last_novel = t
        self.lit_at = {}          # colour -> when they saw it light (awaiting credit)
        self.was_lit = False
        self.boost_until = -1.0   # a nag spurs them for a few seconds
        self.goals_shown = None   # colours the game is showing as goals, if it shows any
        self.done_bias = 0.0      # after a climax, the visit feels finished
        self.knows_twist = False  # read the diorama's signage: Janis is bad, purple frees the spirits
        self.press_at = None      # inside the diorama, about to press "free the spirits"
        self.my_color = None      # the colour of the rope they are at, as they last saw it

    # -- perceiving --------------------------------------------------------------
    def notice(self):
        return self.rng.random() < self.tr.attention

    def feel(self, meaning, family, t, scale=1.0):
        n = self.seen.get(family, 0)
        m = self.seen_meaning.get(meaning, 0)
        impact = IMPACT.get(meaning, 0.0)
        if meaning == "scold":
            # Compliant people dislike being told off; rebels enjoy it.
            impact = 0.44 * (0.5 - self.tr.compliance) if self.tr.compliance < 0.5 else -0.1
        impact *= (0.7 ** n) * (0.9 ** m) * scale
        if n == 0 and meaning not in ("agency", "ambient"):
            impact += 0.08                       # novelty: something they had not seen
            self.last_novel = t
        self.seen[family] = n + 1
        self.seen_meaning[meaning] = m + 1
        self.affect += impact

    def perceive(self, p, world):
        """p: a percept from the world (World._percept)."""
        t = world.t
        kind = p["kind"]
        if kind == "cue":
            if not self.notice():
                return
            meaning, family = p["meaning"], p["family"]
            self.feel(meaning, family, t)
            if meaning in ("praise", "progress", "celebrate"):
                self._credit(t)
            if meaning == "celebrate":
                self.done_bias += 0.5
            if meaning == "nag":
                self.boost_until = t + 5.0
            if meaning == "scold" and self.rope is not None and world.color_of(self.rope) == PURPLE:
                if self.knows_twist:
                    # Her panic is the proof it is working.
                    self.affect += 0.15
                    d = self.tr.learn * 0.5
                else:
                    d = self.tr.learn * (0.5 - self.tr.compliance)
                self.belief[PURPLE] = min(0.98, max(0.02, self.belief[PURPLE] + d))
        elif kind == "setting":
            self.feel(p["meaning"], p["key"] + "=" + p["value"], t, 1.2)
            if p["meaning"] == "celebrate":
                self.done_bias += 0.5
        elif kind == "palette":
            if self.notice():
                self.feel("shift", "palette", t)
        elif kind == "goals":
            self.goals_shown = set(p["colors"]) if self.notice() else self.goals_shown

    def look_in_diorama(self, world):
        """The diorama has opened. Maybe go and look; maybe read the signage.
        Returns True if they went in."""
        go = min(0.95, 0.2 + 0.5 * self.tr.curiosity + 0.3 * self.tr.attention)
        if self.rng.random() >= go:
            return False
        if self.rope is not None:
            world.release(self.rope)
            self.rope = None
        self.walk_until = world.t + self.rng.uniform(15.0, 40.0)   # inside, looking
        if self.rng.random() < 0.5 + 0.4 * self.tr.attention:
            self.learn_twist(world.t, 0.9)
        if self.rng.random() < 0.35 + 0.5 * self.tr.curiosity:     # a button that says "free the spirits"
            self.press_at = world.t + self.rng.uniform(3.0, 12.0)
        return True

    def pressed(self, world):
        """They pressed it. The tubes flash purple and the room says no; an
        attentive presser takes the colour as a hint. A curious one may try
        again while still inside."""
        t = world.t
        self.feel("reveal", "purple_flash", t)
        if not self.knows_twist and self.rng.random() < (0.4 + 0.4 * self.tr.attention) * 0.75:
            self.learn_twist(t, 0.75)
        again = t + self.rng.uniform(3.0, 8.0)
        if again < self.walk_until and self.rng.random() < self.tr.curiosity * 0.5:
            self.press_at = again

    def learn_twist(self, t, sure):
        if self.knows_twist:
            return
        self.knows_twist = True
        self.belief[PURPLE] = max(self.belief[PURPLE], sure)
        self.feel("reveal", "twist", t)

    def _credit(self, t):
        """A praise lands: colours that lit just before are what was wanted."""
        for color, at in list(self.lit_at.items()):
            if t - at <= self.CREDIT_WINDOW:
                mine = self.rope is not None and color == self.my_color
                w = self.tr.learn * (1.0 if mine else self.tr.social)
                self.belief[color] += w * (1 - self.belief[color])
                del self.lit_at[color]

    # -- each tick -----------------------------------------------------------------
    def tick(self, world):
        t, dt = world.t, self.TICK
        # feelings fade; interest follows them and boredom wears it down
        self.fun += self.affect * dt
        self.affect *= math.exp(-dt / self.AFFECT_TAU)
        since = t - self.last_novel
        bored = self.tr.boredom * (1 + since / self.tr.boredom_ramp) * (1 + self.done_bias)
        self.interest += (self.INTEREST_GAIN * self.affect - bored) * dt
        for c in range(len(COLORS)):
            self.belief[c] += (0.5 - self.belief[c]) * (dt / self.FORGET_TAU) * 0.3

        if self.interest <= 0:
            return self.leave(t, "bored")
        if t - self.arrived >= self.tr.budget:
            return self.leave(t, "out_of_time")

        # unclaimed lit colours that drew no praise: probably not wanted
        for color, at in list(self.lit_at.items()):
            if t - at > self.CREDIT_WINDOW:
                self.belief[color] -= self.tr.learn * 0.5 * self.belief[color]
                del self.lit_at[color]

        if self.press_at is not None and t >= self.press_at:
            self.press_at = None
            world.press_button(self)
        if t < self.walk_until:
            return
        if self.rope is None:
            self.rope = world.pick_rope(self)
            if self.rope is None:
                self.walk_until = t + 3.0       # every rope taken: wait a moment
                return

        self._watch(world)
        if world.goals_on_display and self.rng.random() < self.tr.attention * 0.5 * dt:
            self.goals_shown = world.visible_goals()   # a glance at the extractors or the video
        want = self._want(world)
        # Pulling something that seems unwanted: look for another rope, sooner
        # the more curious they are (about every 5-20 s); at once if the game
        # is showing them what it wants.
        switch = 0.05 + 0.15 * self.tr.curiosity
        if self.goals_shown is not None:
            switch = 0.6 * self.tr.compliance + 0.1   # the display says this one is not wanted
        # Theirs is lit and wanted: go and light a wanted colour nobody is on.
        if (want > 0.6 and world.lit(self.rope) and world.charge(self.rope) > 60
                and self.rng.random() < (0.1 + 0.4 * self.tr.attention) * dt):
            target = world.rope_needing(self)
            if target is not None:
                world.release(self.rope)
                world.occupied[target] = self.id
                self.rope = target
                self.walk_until = t + self.rng.uniform(1.0, 2.5)
                return
        if want < 0.35 and self.rng.random() < switch * dt:
            new = (world.rope_needing(self) if self.goals_shown is not None else None) \
                or world.pick_rope(self)
            if new is not None and new != self.rope:
                world.release(self.rope)
                self.rope = new
                self.walk_until = t + self.rng.uniform(1.0, 3.0)
                return

        fatigue = 1.0 / (1.0 + self.pulls / self.tr.stamina)
        # Mostly they pull what they think is wanted; the less compliant also
        # fidget with the rest, just to see.
        mischief = 0.35 * (1.0 - self.tr.compliance)
        keen = want + (1.0 - want) * mischief
        spur = 1.6 if t < self.boost_until else 1.0
        # A colour that is lit and wanted only needs topping up.
        if world.lit(self.rope) and want > 0.6 and world.charge(self.rope) > 70:
            keen *= 0.3
        rate = self.tr.pull_rate * fatigue * keen * spur * min(1.0, 0.4 + self.interest)
        if self.rng.random() < rate * dt:
            self.pulls += 1
            yank = self.rng.random() < self.tr.rough
            if world.pull(self, rough=yank):
                self.feel("agency", "pull", t)          # the handle lights: it counted
            elif yank and self.notice():
                # The handle went dark and nothing happened: ease off.
                self.tr.rough *= 0.5
                self.affect -= 0.03

    def _watch(self, world):
        """The display: did my colour (or one nearby) just light?"""
        color = world.color_of(self.rope)
        if color != self.my_color:
            self.was_lit = False          # a new rope, or the ropes were recoloured
        self.my_color = color
        lit = world.lit(self.rope)
        if lit and not self.was_lit:
            self.feel("lit", "lit", world.t)
            self.lit_at[color] = world.t
        self.was_lit = lit

    def _want(self, world, rope=None):
        return self.want_color(world.color_of(self.rope if rope is None else rope))

    def want_color(self, color):
        if self.goals_shown is not None and color != PURPLE:
            return 0.95 if COLORS[color] in self.goals_shown else 0.15
        return self.belief[color]

    def leave(self, t, why):
        if self.left is None:
            self.left = t
            self.leave_reason = why
