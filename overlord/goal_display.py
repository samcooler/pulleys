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

  EXTRACTION SPEED INCREASED
               2 s, when a round is won (the game moves on to the next round)
  TIME'S UP    2 s, when a round runs out and the game goes back to the start
  MAXIMUM EXTRACTION ACHIEVED
               all through a victory state (VICTORY_STATES: party, the three
               rounds won): a full-screen card with falling confetti, the words
               fading in once and holding. It waits while a clip plays (Janis's
               congratulations first), so she is seen whenever she speaks.

A state change clears whatever the last state left up. A banner shows only for
a round's own ending (won, or its time run out); a state forced by hand from
the debug page gets none.

From the third round's win to victory, purple is the one goal (DISRUPT_STATES:
party, final_wait, final). The first purple pull there replaces all of
the above with the machine's alarm: a red band, EXTRACTION INTEGRITY ...
FAILING, over a purple bar that drains toward zero as the shared purple pool
fills (zero is the win), and PURPLE ACTIVITY DISRUPTS EXTRACTION flashing on every purple
pull. Wrong, as far as the machine is concerned; right, as the bar shows.

In the start state the band asks for a move instead ("MOVE ANY LEVER TO
BEGIN"). In a countdown state (COUNTDOWN_STATES: final_wait, the time after
the party to pull a purple lever, and final, the time to fill the purple bar)
the band holds only the time left before the state's `after`; under the alarm,
that time sits in the middle of its top line.

Goals and states come from the play log, so they work the same whether they
come from the arbiter's telemetry or from overlord's own engine (--game). The
charges come from the bridge's `CO` lines (the arbiter's MeshComponents, see
src/bridge/main.cpp); with an older bridge CURRENT stays empty.
"""

import sys
import math
import random
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
ROUND_WON = "EXTRACTION SPEED\\NINCREASED"
CARD_FADE_S = 0.6
CARD_WORDS = "MAXIMUM\\NEXTRACTION\\NACHIEVED"
REFRESH_S = 1.0            # an overlay is sent again this often even unchanged, in case mpv lost it
CLIP_MAX_S = 40.0
DISRUPT_STATES = ("party", "final_wait", "final")
DISRUPT_FLASH_S = 1.8
PURPLE = "BF5AF2"
ALARM = "FF3B30"
VICTORY_STATES = ("party",)
COUNTDOWN_STATES = ("final_wait", "final")
FPS = 8
CARD_FPS = 20              # while the victory card's confetti falls
CONFETTI_N = 110
CONFETTI_COLORS = ("FF3B30", "0A84FF", "34C759", "FFD60A", "FF9500", "BF5AF2", "FFFFFF", "FF2D95")


def ass_box(x, y, w, h, color, bord, alpha="00", blur=0):
    """An outline of a rectangle (its inside clear), `bord` thick."""
    return (f"{{\\an7\\pos({x},{y})\\bord{bord}\\shad0\\blur{blur}\\1a&HFF&"
            f"\\3c{ass_color(color)}\\3a&H{alpha}&\\p1}}"
            f"m 0 0 l {w} 0 {w} {h} 0 {h}{{\\p0}}")


def ass_banner(text, color, alpha="00", size=190):
    return (f"{{\\an5\\pos({RES_X // 2},{RES_Y // 2})\\fs{size}\\bord8\\shad4\\b1"
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
        except (ValueError, KeyError, TypeError):
            p = {}

        def seconds(v):
            if isinstance(v, str) and v.startswith("$"):
                v = p[v[1:]].value if v[1:] in p else None
            if v is None:
                return None
            try:
                return float(v) if isinstance(v, (int, float)) else parse_duration(v)
            except (ValueError, TypeError):
                return None

        self.round_time = seconds("$round_time")
        # A countdown state's length: (param name or None, seconds).
        self.countdown = {}
        for name in COUNTDOWN_STATES:
            delay = ((cfg["states"].get(name) or {}).get("after") or {}).get("delay")
            if delay is not None:
                param = delay[1:] if isinstance(delay, str) and delay.startswith("$") else None
                self.countdown[name] = (param, seconds(delay))
        self.goals = []
        self.state = None
        self.entered = clock()
        self.prev_entered = self.entered
        # From the bridge's CO lines: per component, its colour name, charge and when.
        self.charge = {}          # colour -> (percent, at)
        self.decay = 0.0          # percent per second
        self.last_rise = {}       # colour -> when its charge last went up
        self.stray_at = {}        # colour -> when it was last pulled as a non-goal
        self.lit = set()          # colours lit: from reaching 100 until drained to 0
        self.banner = None        # (text, colour, until)
        self.disrupted = False    # a purple pull in DISRUPT_STATES: the machine's alarm is up
        self.purple_at = -1e9     # the last purple pull, for the flash
        self.finals = {}          # component index -> (charge, at), the roped purple ones
        self.finals_lit = set()
        self.clip = False         # a video_once clip is playing (set_clip): the card waits
        self.card_t0 = None       # when the card last came up, for its pulse
        rnd = random.Random(7)    # the same confetti every time, a different piece each
        self.confetti = [(rnd.uniform(0, RES_X), rnd.uniform(-RES_Y, 0), rnd.uniform(160, 380),
                          rnd.uniform(20, 90), rnd.uniform(0.6, 2.2), rnd.uniform(0, 6.3),
                          rnd.choice(CONFETTI_COLORS), rnd.uniform(-400, 400), rnd.choice((0, 1)))
                         for _ in range(CONFETTI_N)]
        self.shown = (None, None)
        self.shown_at = [0.0, 0.0]
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
                self.prev_entered, self.entered = self.entered, now
                self._on_state(prev, self.state, now)
            elif kind == "pull" and rec.get("cls") == "nongoal":
                self.stray_at[rec["color"]] = now
            elif kind == "pull" and rec.get("cls") == "final" and self.state in DISRUPT_STATES:
                self.disrupted = True
                self.purple_at = now
            elif kind == "param" and rec.get("name") == "round_time":
                self.round_time = float(rec["value"])
            elif kind == "param":
                for name, (param, _) in self.countdown.items():
                    if param == rec.get("name"):
                        self.countdown[name] = (param, float(rec["value"]))

    def _on_state(self, prev, new, now):
        # Whatever the last state had up goes: its banner, its alarm, its flashes.
        # Only the alarm carries on, through the purple endgame's own states.
        if new not in DISRUPT_STATES or prev not in DISRUPT_STATES:
            self.disrupted = False
        self.banner = None
        self.purple_at = -1e9
        self.stray_at.clear()
        self.card_t0 = None
        if new != prev and (new in self.rounds or new == self.start):
            # The arbiter resets every component on entering these: start the
            # bars from nothing now rather than on its next report.
            self.charge.clear()
            self.last_rise.clear()
            self.lit.clear()
            self.finals.clear()
            self.finals_lit.clear()
        if prev not in self.rounds or new == prev or new in VICTORY_STATES:
            return
        # Only a round's own ending gets a banner. A jump forced by hand is neither.
        i = self.rounds.index(prev)
        if i + 1 < len(self.rounds) and new == self.rounds[i + 1]:
            self.banner = (ROUND_WON, "34C759", now + BANNER_S, 150)
        elif new == self.start and self.round_time and now - self.prev_entered >= self.round_time - 2:
            self.banner = ("TIME'S UP", "FF9500", now + BANNER_S, 190)

    def set_clip(self, playing):
        """overlord.py: a clip started (True) or ended (False)."""
        with self.lock:
            self.clip = self.clock() if playing else False

    def _card_up(self, now):
        # A moment's grace on entry, so the congratulations clip, which starts
        # with the state, takes the screen before the card can.
        # A clip whose end was never heard stops holding it after CLIP_MAX_S.
        clip = self.clip is not False and now - self.clip < CLIP_MAX_S
        return self.state in VICTORY_STATES and not clip and now - self.entered >= 1.0

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
            for i, (q, c) in enumerate(zip(charges, colors)):
                if i < 5 or c != COLORS.index("purple"):
                    continue
                if q >= 100:
                    self.finals_lit.add(i)
                elif q == 0:
                    self.finals_lit.discard(i)
                self.finals[i] = (q, now)

    # ── drawing ──────────────────────────────────────────────────────────────
    def _level(self, name, now):
        """A colour's charge now: the last report, drained since as the arbiter drains it."""
        q, at = self.charge.get(name, (0, now))
        return max(0.0, q - self.decay * (now - at))

    def _clock(self, left, color="FFFFFF"):
        return ass_text(RES_X - 40, BAND_H // 2, f"{int(left) // 60}:{int(left) % 60:02d}", 70, color, align=6)

    def _left(self, now):
        """Seconds left in a countdown state, or None."""
        _, total = self.countdown.get(self.state, (None, None))
        return max(0, total - (now - self.entered)) if total else None

    def _band(self, now):
        state = self.state
        left = self._left(now)
        if left is not None:
            return [ass_rect(0, 0, RES_X, BAND_H, "000000", alpha="50"), self._clock(left)]
        if state not in self.goal_states and state != self.start:
            return []
        lines = [ass_rect(0, 0, RES_X, BAND_H, "000000", alpha="50")]
        if state == self.start or not self.goals:
            lines.append(ass_text(RES_X // 2, BAND_H // 2, "MOVE ANY LEVER TO BEGIN", 78, align=5))
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
            lines.append(self._clock(left, "FF3B30" if urgent else "FFFFFF"))
        return lines

    def _banner(self, now):
        if not self.banner:
            return []
        text, color, until, size = self.banner
        if now >= until:
            self.banner = None
            return []
        # Fade out over the last half second.
        left = until - now
        alpha = "00" if left > 0.5 else f"{int((0.5 - left) / 0.5 * 255):02X}"
        h = 300 if "\\N" not in text else 440
        return [ass_rect(0, RES_Y // 2 - h // 2, RES_X, h, "000000", alpha="60"),
                ass_banner(text, color, alpha, size)]

    def _card(self, now):
        """The party card: a deep purple screen, the words in gold, confetti
        falling over it. It fades in and holds."""
        if not self._card_up(now):
            self.card_t0 = None
            return []
        if self.card_t0 is None:
            self.card_t0 = now
        t = now - self.card_t0
        fade = min(1.0, t / CARD_FADE_S)
        if fade <= 0:
            return []
        a = f"{int((1 - fade) * 255):02X}"
        lines = [ass_rect(0, 0, RES_X, RES_Y, "1E0B3A", alpha=f"{max(0x18, int((1 - fade) * 255)):02X}")]
        # A slow throb on the word, so it feels alive rather than printed.
        scale = 100 + 6 * math.sin(t * 4)
        lines.append(f"{{\\an5\\pos({RES_X // 2},{RES_Y // 2})\\fs190\\b1\\bord10\\shad8"
                     f"\\fscx{scale:.0f}\\fscy{scale:.0f}\\1c{ass_color('FFD60A')}\\3c{ass_color('7A3E00')}"
                     f"\\4c&H000000&\\1a&H{a}&\\3a&H{a}&\\4a&H80&}}{CARD_WORDS}")
        for x0, y0, speed, sway, freq, phase, color, spin, shape in self.confetti:
            y = (y0 + speed * t) % (RES_Y + 120) - 60
            x = x0 + sway * math.sin(freq * t + phase)
            w, h = (14, 30) if shape else (22, 22)
            # A flip: the piece's width swings through zero as it tumbles.
            fx = abs(math.cos(freq * 1.7 * t + phase)) * 100
            lines.append(f"{{\\an5\\pos({x:.0f},{y:.0f})\\bord0\\shad0\\frz{(spin * t) % 360:.0f}"
                         f"\\fscx{fx:.0f}\\1c{ass_color(color)}\\1a&H{a}&\\p1}}"
                         f"m 0 0 l {w} 0 {w} {h} 0 {h}{{\\p0}}")
        return lines

    def _integrity(self, now):
        """What is left of the extraction, 1..0: each roped purple channel counts
        fully once lit (it stays lit until it has drained to nothing, as the
        arbiter rules), else by its charge. 0 is every one lit: the win."""
        if not self.finals:
            return 1.0
        done = 0.0
        for i, (q, at) in self.finals.items():
            level = max(0.0, q - self.decay * (now - at))
            done += 1.0 if (i in self.finals_lit and level > 0) else level / 100
        return max(0.0, 1.0 - done / len(self.finals))

    def _alarm_band(self, now):
        left = self._integrity(now)
        hot = now - self.purple_at < DISRUPT_FLASH_S
        blink = int(now * 3) % 2 == 0
        lines = [ass_rect(0, 0, RES_X, BAND_H, "1A0000", alpha="20")]
        lines.append(ass_box(0, 0, RES_X, BAND_H, ALARM, 10 if hot else 6,
                             "00" if (hot and int(now * 6) % 2 == 0) else "50"))
        # No number: a draining bar reads as "less of the machine" at a glance,
        # where a percentage reads as a score that should climb.
        lines.append(ass_text(40, 18, "EXTRACTION INTEGRITY", 58))
        lines.append(ass_text(RES_X - 40, 18, "FAILING", 58, ALARM if blink else "FFFFFF", align=9))
        time_left = self._left(now)
        if time_left is not None:
            lines.append(ass_text(RES_X // 2, 14, f"{int(time_left) // 60}:{int(time_left) % 60:02d}", 64,
                                  ALARM if time_left <= 20 else "FFFFFF", align=8))
        x, y, w, h = 40, 92, RES_X - 80, 52
        lines.append(ass_rect(x, y, w, h, "000000", alpha="20"))
        if left > 0:
            lines.append(ass_rect(x, y, max(2, int(w * left)), h, PURPLE))
        lines.append(ass_box(x, y, w, h, "FFFFFF", 3))
        return lines

    def _alarm_flash(self, now):
        t = now - self.purple_at
        if t >= DISRUPT_FLASH_S:
            return []
        a = "00" if t < DISRUPT_FLASH_S - 0.4 else f"{int((t - (DISRUPT_FLASH_S - 0.4)) / 0.4 * 255):02X}"
        jx, jy = random.randint(-8, 8), random.randint(-5, 5)
        cx, cy = RES_X // 2 + jx, RES_Y // 2 + jy
        lines = [ass_rect(0, RES_Y // 2 - 170, RES_X, 340, "000000", alpha="50")]
        text = "PURPLE ACTIVITY\\NDISRUPTS EXTRACTION"
        # A red ghost a few pixels off: the picture glitching.
        lines.append(f"{{\\an5\\pos({cx + 9},{cy + 5})\\fs120\\b1\\bord0\\shad0"
                     f"\\1c{ass_color(ALARM)}\\1a&H{'60' if a == '00' else a}&}}{text}")
        lines.append(f"{{\\an5\\pos({cx},{cy})\\fs120\\b1\\bord6\\shad0"
                     f"\\1c{ass_color(PURPLE)}\\3c&H000000&\\1a&H{a}&\\3a&H{a}&}}{text}")
        return lines

    def _show(self, oid, lines, index):
        text = "\n".join(lines)
        now = self.clock()
        if text == self.shown[index] and (not text or now - self.shown_at[index] < REFRESH_S):
            return
        self.shown_at[index] = now
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
            if self.disrupted and self.state in DISRUPT_STATES:
                band, banner = self._alarm_band(now), self._alarm_flash(now)
            else:
                banner = self._card(now) if self.state in VICTORY_STATES else self._banner(now)
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
            time.sleep(1 / (CARD_FPS if self.state in VICTORY_STATES or self.disrupted else FPS))
