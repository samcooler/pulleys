"""Simulator: stand-in for the real components, driven from the keyboard.

Keys 1-5 light a major component, each in its own colour, and keys 6-8 light
the final components, all purple. A lit component wears out after WEAR
seconds; pressing its key again refills it. Lighting and wearing out send the same game events the
real hardware will, and an overlay on the bottom half of the video shows each
component's remaining charge as a bar.

Optional: overlord.py only imports this with --simulate.
"""

import os
import tempfile
import threading
import time

WEAR = 5.0  # seconds a component stays lit
FPS = 10

# (key, colour as RRGGBB)
MAJOR = [
    ("1", "FF3B30"),  # red
    ("2", "FF9500"),  # orange
    ("3", "FFCC00"),  # yellow
    ("4", "34C759"),  # green
    ("5", "0A84FF"),  # blue
]
FINAL_KEYS = ["6", "7", "8"]
FINAL_COLOR = "AF52DE"  # purple

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


class Component:
    def __init__(self, key, color, final):
        self.key = key
        self.color = color
        self.final = final
        self.lit_until = 0.0

    def charge(self, now):
        return max(0.0, min(1.0, (self.lit_until - now) / WEAR))

    def lit(self, now):
        return self.lit_until > now


class Simulator:
    def __init__(self, game, send_event):
        """send_event: callback(event_name) that feeds the game engine."""
        self.game = game
        self.send_event = send_event
        self.video = None
        self.majors = [Component(k, c, False) for k, c in MAJOR]
        self.finals = [Component(k, FINAL_COLOR, True) for k in FINAL_KEYS]
        self.by_key = {c.key: c for c in self.majors + self.finals}
        self._lock = threading.Lock()
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
        print(f"simulate: keys {MAJOR[0][0]}-{MAJOR[-1][0]} light major components, "
              f"{FINAL_KEYS[0]}-{FINAL_KEYS[-1]} the final ones (focus the video window)",
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
        comp = self.by_key.get(key)
        if comp is None:
            return
        events = []
        with self._lock:
            now = time.monotonic()
            was_lit = comp.lit(now)
            comp.lit_until = now + WEAR
            if not was_lit:
                print(f"simulate: component {key} lit", flush=True)
                if comp.final:
                    events.append("final_component_activated")
                    if all(c.lit(now) for c in self.finals):
                        events.append("all_final_components_activated")
                else:
                    events.append("component_activated")
                    if all(c.lit(now) for c in self.majors):
                        events.append("all_components_activated")
        self._send(events)

    def _tick_loop(self):
        while self._running:
            events = []
            with self._lock:
                now = time.monotonic()
                for comp in self.majors + self.finals:
                    # lit_until is cleared once the wear-out has been reported
                    if comp.lit_until and not comp.lit(now):
                        comp.lit_until = 0.0
                        print(f"simulate: component {comp.key} wore out", flush=True)
                        if comp.final:
                            events.append("final_component_deactivated")
                        else:
                            events.append("component_deactivated")
                            if not any(c.lit(now) for c in self.majors):
                                events.append("all_components_deactivated")
                overlay = self._render(now)
            self._send(events)
            try:
                self.video.command("osd-overlay", OVERLAY_ID, "ass-events", overlay,
                                   RES_X, RES_Y, 10)
            except Exception as e:
                print(f"simulate: overlay failed: {e}", flush=True)
            time.sleep(1.0 / FPS)

    def _send(self, events):
        for name in events:
            self.send_event(name)

    def _show_finals(self):
        # States that react to the final components show their bars instead of the majors.
        return any(self.game.reacts_to(e) for e in (
            "final_component_activated", "final_component_deactivated",
            "all_final_components_activated"))

    def _render(self, now):
        comps = self.finals if self._show_finals() else self.majors

        lines = [ass_rect(0, TOP, RES_X, RES_Y - TOP, "000000", alpha="70")]
        lines.append(ass_text(MARGIN, TOP + 20, (self.game.state or "").upper(), 40))

        bar_x = MARGIN + LABEL_W
        bar_w = RES_X - bar_x - MARGIN
        y = TOP + 80
        for comp in comps:
            lines.append(ass_text(MARGIN + LABEL_W // 2, y + BAR_H // 2, comp.key, 40,
                                  comp.color, align=5))
            lines.append(ass_rect(bar_x, y, bar_w, BAR_H, "FFFFFF", alpha="D8"))
            fill = int(bar_w * comp.charge(now))
            if fill > 0:
                lines.append(ass_rect(bar_x, y, fill, BAR_H, comp.color))
            y += BAR_H + BAR_GAP
        return "\n".join(lines)
