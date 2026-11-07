"""Shows the game's goal colours over the video, for games with show_goals.

A Simon round asks for colours nobody can see unless something shows them;
playtesting in simulation found most visitors never work them out from
Janis's reactions alone (playtest/README.md). A game that sets
`show_goals: true` gets a band across the top of the video while a state that
cares about goals is running: one block per wanted colour.

It listens to the play log, so it works the same whether the goals come from
the arbiter's telemetry or from overlord's own engine (--game).
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from playtest.spec import reactions  # noqa: E402
from simulate import COLOR_RGB, RES_X, RES_Y, ass_rect, ass_text  # noqa: E402

OVERLAY_ID = 8
GOAL_EVENTS = ("goal_pull", "nongoal_pull", "goal_component_activated",
               "first_goal_component_activated", "all_components_activated")
BAND_H = 150


class GoalDisplay:
    def __init__(self, cfg, video):
        self.video = video
        # States in which goals mean something: they react to a goal event.
        self.goal_states = {name for name, st in cfg["states"].items()
                            if any(e in reactions(st or {}) for e in GOAL_EVENTS)}
        self.goals = []
        self.state = None

    def on_record(self, rec):
        kind = rec["kind"]
        if kind == "goals":
            self.goals = list(rec["colors"])
        elif kind == "state":
            self.state = rec["name"]
        else:
            return
        self.draw()

    def draw(self):
        show = self.state in self.goal_states and self.goals
        try:
            if not show:
                self.video.command("osd-overlay", OVERLAY_ID, "none", "")
                return
            lines = [ass_rect(0, 0, RES_X, BAND_H, "000000", alpha="60")]
            lines.append(ass_text(48, BAND_H // 2, "PULL", 64, align=4))
            w, gap = 220, 30
            x = 48 + 230
            for c in self.goals:
                lines.append(ass_rect(x, 25, w, BAND_H - 50, COLOR_RGB.get(c, "FFFFFF")))
                x += w + gap
            self.video.command("osd-overlay", OVERLAY_ID, "ass-events", "\n".join(lines),
                               RES_X, RES_Y, 20)
        except Exception as e:
            print(f"goals: overlay failed: {e}", flush=True)
