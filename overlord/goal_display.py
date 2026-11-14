"""The game's status over the video, for games with show_goals.

Playtesting found most visitors never work out a Simon round from Janis's
reactions alone (playtest/README.md), so a game that sets `show_goals: true`
gets a band across the top of the video:

    ROUND 1 of 3   GOAL [red] [blue]   CURRENT [blue|||] [green|]      1:42

  GOAL     the colours wanted, solid; outlined white once that one is full.
  CURRENT  every colour with charge, most recently pulled first, filled to its
           charge. A colour that is not a goal glows red, brighter just after
           a pull of it.
  right    the round's time left.

And big words in the middle of the screen when the game moves:

  SUCCESS!     2 s, when a round is won (it leaves for anything but the start)
  TIME'S UP    2 s, when a round runs out and the game goes back to the start
  VICTORY!     on entering a victory state (VICTORY_STATES)

In the start state the band asks for a pull instead ("PULL ANY LEVER TO
BEGIN").

Goals and states come from the play log, so they work the same whether they
come from the arbiter's telemetry or from overlord's own engine (--game). The
charges come from the bridge's `CO` lines (the arbiter's MeshComponents, see
src/bridge/main.cpp); with an older bridge CURRENT stays empty.
"""

import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from playtest.spec import goal_states, load_params, parse_duration  # noqa: E402
from playlog import COLORS  # noqa: E402
from simulate import COLOR_RGB, RES_X, RES_Y, ass_color, ass_rect, ass_text  # noqa: E402

BAND_ID = 8       # the band; the debug menu shares it, and is only up when this is not wanted
BANNER_ID = 11    # the big words, above the band and the messages (9)
BAND_H = 170
BLOCK_W, BLOCK_H, GAP = 130, 104, 18
BLOCK_Y = (BAND_H - BLOCK_H) // 2
WRONG = "FF3B30"
WRONG_FLASH_S = 1.5        # a stray pull's glow, brightest right after it
BANNER_S = 2.0
VICTORY_S = 6.0
VICTORY_STATES = ("victory2",)
FPS = 8


def ass_box(x, y, w, h, color, bord, alpha="00", blur=0):
    """An outline of a rectangle (its inside clear), `bord` thick."""
    return (f"{{\\an7\\pos({x},{y})\\bord{bord}\\shad0\\blur{blur}\\1a&HFF&"
            f"\\3c{ass_color(color)}\\3a&H{alpha}&\\p1}}"
            f"m 0 0 l {w} 0 {w} {h} 0 {h}{{\\p0}}")


def ass_banner(text, color, alpha="00"):
    return (f"{{\\an5\\pos({RES_X // 2},{RES_Y // 2})\\fs190\\bord8\\shad4\\b1"
            f"\\1c{ass_color(color)}\\3c&H000000&\\1a&H{alpha}&\\3a&H{alpha}&}}{text}")


class GoalDisplay:
    def __init__(self, cfg, video, clock=time.monotonic):
        self.video = video
        self.clock = clock
        self.lock = threading.Lock()
        states = list(cfg["states"])
        self.start = cfg.get("start")
        gs = goal_states(cfg)
        self.goal_states = gs
        # The rounds, in the order the game lists them: goal states but the start.
        self.rounds = [s for s in states if s in gs and s != self.start]
        try:
            p = load_params(cfg)
            v = p["round_time"].value if "round_time" in p else None
            self.round_time = (float(v) if isinstance(v, (int, float)) else parse_duration(v)) if v is not None else None
        except (ValueError, KeyError, TypeError):
            self.round_time = None
        self.goals = []
        self.state = None
        self.entered = clock()
        # From the bridge's CO lines: per component, its colour name, charge and when.
        self.charge = {}          # colour -> (percent, at)
        self.decay = 0.0          # percent per second
        self.last_rise = {}       # colour -> when its charge last went up
        self.stray_at = {}        # colour -> when it was last pulled as a non-goal
        self.lit = set()          # colours lit: from reaching 100 until drained to 0
        self.banner = None        # (text, colour, until)
        self.shown = (None, None)
        threading.Thread(target=self._run, daemon=True, name="goal-display").start()

    # ── inputs ───────────────────────────────────────────────────────────────
    def on_record(self, rec):
        kind = rec["kind"]
        now = self.clock()
        with self.lock:
            if kind == "goals":
                self.goals = list(rec["colors"])
            elif kind == "state":
                prev = rec.get("prev") or self.state
                self.state = rec["name"]
                self.entered = now
                self._on_state(prev, self.state, now)
            elif kind == "pull" and rec.get("cls") == "nongoal":
                self.stray_at[rec["color"]] = now
            elif kind == "param" and rec.get("name") == "round_time":
                self.round_time = float(rec["value"])

    def _on_state(self, prev, new, now):
        if new in VICTORY_STATES:
            self.banner = ("VICTORY!", "FFD60A", now + VICTORY_S)
        elif prev in self.rounds and new != prev:
            if new == self.start:
                self.banner = ("TIME'S UP", "FF9500", now + BANNER_S)
            else:
                self.banner = ("SUCCESS!", "34C759", now + BANNER_S)

    def on_components(self, line):
        """`CO <decay> <charges> <colours> <goals>` from the bridge."""
        try:
            _, decay, charges, colors, _goals = line.split()
            charges = [int(x) for x in charges.split(",")]
            colors = [int(x) for x in colors.split(",")]
        except ValueError:
            return
        now = self.clock()
        with self.lock:
            self.decay = int(decay) / 1000.0
            seen = {}
            for q, c in zip(charges, colors):
                if 0 <= c < len(COLORS):
                    name = COLORS[c]
                    seen[name] = max(seen.get(name, 0), q)   # the finals share purple
            for name, q in seen.items():
                if q > self._level(name, now) + 1:
                    self.last_rise[name] = now
                if q >= 100:
                    self.lit.add(name)
                elif q == 0:
                    self.lit.discard(name)
                self.charge[name] = (q, now)

    # ── drawing ──────────────────────────────────────────────────────────────
    def _level(self, name, now):
        """A colour's charge now: the last report, drained since as the arbiter drains it."""
        q, at = self.charge.get(name, (0, now))
        return max(0.0, q - self.decay * (now - at))

    def _band(self, now):
        state = self.state
        if state not in self.goal_states:
            return []
        lines = [ass_rect(0, 0, RES_X, BAND_H, "000000", alpha="50")]
        if state == self.start or not self.goals:
            lines.append(ass_text(RES_X // 2, BAND_H // 2, "PULL ANY LEVER TO BEGIN", 78, align=5))
            return lines
        x = 40
        if state in self.rounds:
            lines.append(ass_text(x, 22, "ROUND", 34, "8B949E"))
            lines.append(ass_text(x, 62, f"{self.rounds.index(state) + 1} of {len(self.rounds)}", 56))
            x += 210
        lines.append(ass_text(x, BAND_H // 2, "GOAL", 50, align=4))
        x += 160
        for c in self.goals:
            lines.append(ass_rect(x, BLOCK_Y, BLOCK_W, BLOCK_H, COLOR_RGB.get(c, "FFFFFF")))
            if c in self.lit and self._level(c, now) > 0:
                lines.append(ass_box(x, BLOCK_Y, BLOCK_W, BLOCK_H, "FFFFFF", 7))
            x += BLOCK_W + GAP
        x += 50
        lines.append(ass_text(x, BAND_H // 2, "CURRENT", 50, align=4))
        x += 260
        active = [c for c in COLORS[:5] if self._level(c, now) > 0.5]
        active.sort(key=lambda c: -self.last_rise.get(c, 0))
        for c in active:
            if x + BLOCK_W > RES_X - 200:
                break
            color = COLOR_RGB.get(c, "FFFFFF")
            if c not in self.goals:
                since = now - self.stray_at.get(c, -1e9)
                hot = since < WRONG_FLASH_S
                pulse = (int(now * 4) % 2 == 0) if hot else False
                # A soft red halo, and a hard red frame inside it: "not this one".
                lines.append(ass_box(x, BLOCK_Y, BLOCK_W, BLOCK_H, WRONG,
                                     22 if hot else 14, "00" if hot and pulse else "20", blur=16))
            level = self._level(c, now)
            fill = int(BLOCK_H * min(level, 100) / 100)
            lines.append(ass_rect(x, BLOCK_Y, BLOCK_W, BLOCK_H, "000000", alpha="30"))
            if fill:
                lines.append(ass_rect(x, BLOCK_Y + BLOCK_H - fill, BLOCK_W, fill, color))
            lines.append(ass_box(x, BLOCK_Y, BLOCK_W, BLOCK_H, WRONG if c not in self.goals else color,
                                 6 if c not in self.goals else 4))
            x += BLOCK_W + GAP
        if self.round_time and state in self.rounds:
            left = max(0, self.round_time - (now - self.entered))
            urgent = left <= 20
            lines.append(ass_text(RES_X - 40, BAND_H // 2, f"{int(left) // 60}:{int(left) % 60:02d}",
                                  70, "FF3B30" if urgent else "FFFFFF", align=6))
        return lines

    def _banner(self, now):
        if not self.banner:
            return []
        text, color, until = self.banner
        if now >= until:
            self.banner = None
            return []
        # Fade out over the last half second.
        left = until - now
        alpha = "00" if left > 0.5 else f"{int((0.5 - left) / 0.5 * 255):02X}"
        return [ass_rect(0, RES_Y // 2 - 150, RES_X, 300, "000000", alpha="60"),
                ass_banner(text, color, alpha)]

    def _show(self, oid, lines, index):
        text = "\n".join(lines)
        if text == self.shown[index]:
            return
        if text:
            self.video.command("osd-overlay", oid, "ass-events", text, RES_X, RES_Y, 20 + index)
        else:
            self.video.command("osd-overlay", oid, "none", "")
        shown = list(self.shown)
        shown[index] = text
        self.shown = tuple(shown)

    def draw(self):
        now = self.clock()
        with self.lock:
            banner = self._banner(now)
            # A banner has the screen to itself: the band (the next round's goals) waits for it.
            band = [] if banner else self._band(now)
        try:
            self._show(BAND_ID, band, 0)
            self._show(BANNER_ID, banner, 1)
        except Exception as e:
            print(f"goals: overlay failed: {e}", flush=True)

    def _run(self):
        while True:
            self.draw()
            time.sleep(1 / FPS)
