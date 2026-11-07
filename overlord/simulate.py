"""Simulator: stand-in for the ropes, driven from the keyboard.

Keys 1-5 pull a rope of each major colour, in the game's order, and keys 6-8
three purple ropes. A press is a pull on that rope's channel, handed to the
game's engine (playtest/engine.py), which charges, lights and wears out the
components by the arbiter's rules and sends the same game events -- pull
events included. An overlay on the bottom half of the video shows each
component's charge as a bar, and the goals the game has dealt.

Optional: overlord.py only imports this with --simulate.
"""

import os
import tempfile
import threading
import time

FPS = 10

# (key, colour as RRGGBB)
# The default game's majors, in its order (state_config.yaml: red, blue, green,
# yellow, orange), so a key and the rope colour it stands in for agree.
MAJOR = [
    ("1", "FF3B30"),  # red
    ("2", "0A84FF"),  # blue
    ("3", "34C759"),  # green
    ("4", "FFD60A"),  # yellow
    ("5", "FF9500"),  # orange
]
FINAL_KEYS = ["6", "7", "8"]
FINAL_COLOR = "AF52DE"  # purple
COLOR_RGB = {"red": "FF3B30", "blue": "0A84FF", "green": "34C759", "yellow": "FFD60A",
             "orange": "FF9500", "purple": FINAL_COLOR}
spec_colors = ("red", "blue", "green", "yellow", "orange", "purple")
PURPLE = 5

# Overlay layout, in a virtual 1920x1080 screen scaled to the window.
RES_X, RES_Y = 1920, 1080
TOP = RES_Y // 2
MARGIN = 48
LABEL_W = 70
BAR_H = 48
BAR_GAP = 14
OVERLAY_ID = 7


def ass_color(rrggbb):
    """RRGGBB -> ASS &HBBGGRR&."""
    return f"&H{rrggbb[4:6]}{rrggbb[2:4]}{rrggbb[0:2]}&"


def ass_rect(x, y, w, h, color, alpha="00"):
    """A filled rectangle as one ASS event line. alpha 00 is opaque, FF clear."""
    return (f"{{\\an7\\pos({x},{y})\\bord0\\shad0\\1c{ass_color(color)}\\1a&H{alpha}&\\p1}}"
            f"m 0 0 l {w} 0 {w} {h} 0 {h}{{\\p0}}")


def ass_text(x, y, text, size, color="FFFFFF", align=7):
    return (f"{{\\an{align}\\pos({x},{y})\\fs{size}\\bord2\\shad0\\b1"
            f"\\1c{ass_color(color)}\\3c&H000000&}}{text}")


class Simulator:
    def __init__(self, game, send_event=None):
        """game: overlord's Game. Each key pulls a rope channel through the
        game's engine (playtest/engine.py), which does the charging, lighting
        and wearing out exactly as the arbiter does. send_event is unused, kept
        for callers of the old signature."""
        self.game = game
        self.video = None
        eng = game.engine
        roped = sorted(eng.roped)
        # Keys 1-5: a rope of each major's colour, in the game's order.
        # Keys 6-8: the first three purple ropes.
        self.majors = []
        for i, (key, rgb) in enumerate(MAJOR):
            color = eng.major_color[i]
            ch = next((c for c in roped if eng.palette[c] == color), None)
            self.majors.append((key, ch, COLOR_RGB.get(spec_colors[color], rgb)))
        purple = [c for c in roped if eng.palette[c] == PURPLE][:len(FINAL_KEYS)]
        self.finals = [(k, ch, FINAL_COLOR) for k, ch in zip(FINAL_KEYS, purple)]
        self.by_key = {k: ch for k, ch, _ in self.majors + self.finals if ch is not None}
        self._running = False
        self._input_conf = None

    def mpv_args(self):
        """Extra args for the video mpv: binds the keys to client messages."""
        fd, path = tempfile.mkstemp(prefix="overlord-sim-", suffix=".conf")
        with os.fdopen(fd, "w") as f:
            for key in self.by_key:
                f.write(f"{key} script-message sim-key {key}\n")
        self._input_conf = path
        return [f"--input-conf={path}"]

    def start(self, video):
        self.video = video
        self._running = True
        threading.Thread(target=self._tick_loop, daemon=True).start()
        print(f"simulate: keys {MAJOR[0][0]}-{MAJOR[-1][0]} pull a rope of each major colour, "
              f"{FINAL_KEYS[0]}-{FINAL_KEYS[-1]} the purple ones (focus the video window)",
              flush=True)

    def stop(self):
        self._running = False
        if self._input_conf:
            try:
                os.unlink(self._input_conf)
            except FileNotFoundError:
                pass

    def on_mpv_event(self, ev):
        args = ev.get("args") or []
        if ev.get("event") == "client-message" and len(args) == 2 and args[0] == "sim-key":
            self.press(args[1])

    def press(self, key):
        ch = self.by_key.get(key)
        if ch is not None:
            self.game.pull(ch)

    def _tick_loop(self):
        while self._running:
            overlay = self._render()
            try:
                self.video.command("osd-overlay", OVERLAY_ID, "ass-events", overlay,
                                   RES_X, RES_Y, 10)
            except Exception as e:
                print(f"simulate: overlay failed: {e}", flush=True)
            time.sleep(1.0 / FPS)

    def _show_finals(self):
        # States that react to the final components show their bars instead of the majors.
        return any(self.game.reacts_to(e) for e in (
            "final_component_activated", "final_component_deactivated",
            "all_final_components_activated", "final_pull"))

    def _render(self):
        eng = self.game.engine
        with self.game._lock:
            rows = self.finals if self._show_finals() else self.majors
            bars = [(key, rgb, eng.charge_of_channel(ch) / 100.0 if ch is not None else 0.0)
                    for key, ch, rgb in rows]
            state = self.game.state or ""
            goals = eng.goal_colors()

        lines = [ass_rect(0, TOP, RES_X, RES_Y - TOP, "000000", alpha="70")]
        title = state.upper() + (f"   goals: {' '.join(goals)}" if goals else "")
        lines.append(ass_text(MARGIN, TOP + 20, title, 40))

        bar_x = MARGIN + LABEL_W
        bar_w = RES_X - bar_x - MARGIN
        y = TOP + 80
        for key, rgb, charge in bars:
            lines.append(ass_text(MARGIN + LABEL_W // 2, y + BAR_H // 2, key, 40, rgb, align=5))
            lines.append(ass_rect(bar_x, y, bar_w, BAR_H, "FFFFFF", alpha="D8"))
            fill = int(bar_w * max(0.0, min(1.0, charge)))
            if fill > 0:
                lines.append(ass_rect(bar_x, y, fill, BAR_H, rgb))
            y += BAR_H + BAR_GAP
        return "\n".join(lines)
