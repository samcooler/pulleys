"""A small spinner in the bottom-right corner of the video while firmware is
being flashed, so anyone looking at the screen knows the installation is busy
and will come back by itself.

Draws on mpv's on-screen-display layer, like goal_display, and only while the
firmware manager has an update job running (not a scan, not an idle queue).
"""
import math
import threading
import time

from simulate import RES_X, RES_Y, ass_text

OVERLAY_ID = 9
DOTS = 12
RADIUS = 26          # of the ring, in video pixels
DOT = 6              # half the size of one dot
CENTER = (RES_X - 70, RES_Y - 70)
TURN_S = 1.2         # seconds per revolution
FRAME_S = 0.08


def spinner_events(phase):
    """ASS lines for one frame: a ring of dots with a bright head and a fading
    tail. phase is turns, so 0.25 is a quarter of the way round."""
    cx, cy = CENTER
    lines = []
    for i in range(DOTS):
        angle = 2 * math.pi * i / DOTS - math.pi / 2
        behind = (phase * DOTS - i) % DOTS          # 0 at the head, up to DOTS at the tail
        alpha = int(min(1.0, behind / DOTS) * 0.85 * 255)
        x, y = cx + RADIUS * math.cos(angle), cy + RADIUS * math.sin(angle)
        d = DOT
        path = f"m {x:.1f} {y - d:.1f} l {x + d:.1f} {y:.1f} {x:.1f} {y + d:.1f} {x - d:.1f} {y:.1f}"
        lines.append(f"{{\\an7\\pos(0,0)\\bord0\\shad0\\1c&HFFFFFF&\\1a&H{alpha:02X}&\\p1}}{path}{{\\p0}}")
    lines.append(ass_text(cx - RADIUS - 24, cy, "UPDATING FIRMWARE", 30, align=6))
    return lines


class UpdateOverlay:
    def __init__(self, video, is_updating):
        self.video = video
        self.is_updating = is_updating     # a cheap callable: is an update job running?
        self.shown = False
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, name="update-overlay", daemon=True)
        self._thread.start()

    def _run(self):
        while True:
            try:
                self.tick(time.monotonic())
            except Exception as e:         # the show goes on without it
                print(f"update overlay: {e}", flush=True)
            time.sleep(FRAME_S)

    def tick(self, now):
        if self.is_updating():
            self.video.command("osd-overlay", OVERLAY_ID, "ass-events",
                               "\n".join(spinner_events((now / TURN_S) % 1.0)), RES_X, RES_Y, 30)
            self.shown = True
        elif self.shown:
            self.video.command("osd-overlay", OVERLAY_ID, "none", "")
            self.shown = False
