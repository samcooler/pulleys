#!/usr/bin/env python3
"""Draw a game state config as a flow chart.

    state_chart.py [config.yaml] [-o out.png] [-f png|svg|pdf]

Reads game_state_machine/state_config.yaml by default and writes the image
next to it. Needs the graphviz `dot` binary (brew install graphviz) and the
python packages `graphviz` and `pyyaml`.

Each state is a card listing every cue it fires: on entry, and per event.
Arrows between states are labelled with the event, or `after <delay>` for a
timer. An event that fires cues without leaving the state is a loop arrow back
onto that state.
"""

import argparse
import os
import sys

try:
    import graphviz
    import yaml
except ImportError:
    sys.stderr.write("missing deps; run: pip3 install graphviz pyyaml\n")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(ROOT, "game_state_machine", "state_config.yaml")

FONT = "Helvetica"
INK = "#1f2937"
MUTED = "#6b7280"
HEADER = "#3730a3"
START_HEADER = "#111827"
CARD_FILL = "#ffffff"
CARD_BORDER = "#9ca3af"
EVENT_COLOR = "#1f2937"
TIMER_COLOR = "#b45309"
LOOP_COLOR = "#7c3aed"
VIDEO_COLOR = "#1d4ed8"
AUDIO_COLOR = "#047857"
BAND_FILL = "#f3f4f6"


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def reactions(state):
    # YAML 1.1 reads a bare `on:` key as boolean True
    return state.get("on") or state.get(True) or {}


def cue_cell(cue):
    """One cue, coloured by video/audio and marked loop or once."""
    color = VIDEO_COLOR if cue.startswith("video_") else AUDIO_COLOR
    mark = "↻" if "_loop_" in cue else "▸"
    return (f'<TD ALIGN="LEFT" BORDER="0" CELLPADDING="2">'
            f'<FONT POINT-SIZE="11" COLOR="{color}">{mark}  {esc(cue)}</FONT></TD>')


def section_rows(title, cues):
    rows = [f'<TR><TD ALIGN="LEFT" BGCOLOR="{BAND_FILL}" BORDER="0" CELLPADDING="3">'
            f'<FONT POINT-SIZE="10" COLOR="{MUTED}"><B>{esc(title)}</B></FONT></TD></TR>']
    rows += [f"<TR>{cue_cell(c)}</TR>" for c in cues]
    return rows


def state_label(name, state, is_start):
    header = START_HEADER if is_start else HEADER
    title = f'<FONT POINT-SIZE="17" COLOR="white"><B>{esc(name.upper())}</B></FONT>'
    if is_start:
        title += '<BR/><FONT POINT-SIZE="9" COLOR="#d1d5db">START</FONT>'
    rows = [f'<TR><TD BGCOLOR="{header}" CELLPADDING="8">{title}</TD></TR>']
    if state.get("on_enter_cue"):
        rows += section_rows("ON ENTER", state["on_enter_cue"])
    for event, action in reactions(state).items():
        cues = (action or {}).get("cue") or []
        if cues:
            rows += section_rows(f"ON {event}", cues)
    return ('<<TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="4" '
            f'COLOR="{CARD_BORDER}" BGCOLOR="{CARD_FILL}" STYLE="ROUNDED">'
            + "".join(rows) + "</TABLE>>")


def legend_label():
    def row(text, color):
        return (f'<TR><TD ALIGN="LEFT" BORDER="0"><FONT POINT-SIZE="10" COLOR="{color}">'
                f"{text}</FONT></TD></TR>")
    rows = [
        row("■  video cue", VIDEO_COLOR),
        row("■  audio cue", AUDIO_COLOR),
        row("↻  loops until changed", MUTED),
        row("▸  plays once", MUTED),
        row("──▶  event", EVENT_COLOR),
        row("- - ▶  timer", TIMER_COLOR),
        row("↺  event, stays in state", LOOP_COLOR),
    ]
    return ('<<TABLE BORDER="1" CELLBORDER="0" CELLSPACING="0" CELLPADDING="3" '
            f'COLOR="{CARD_BORDER}" STYLE="ROUNDED">'
            f'<TR><TD ALIGN="LEFT" BORDER="0"><FONT POINT-SIZE="10" COLOR="{MUTED}">'
            "<B>KEY</B></FONT></TD></TR>" + "".join(rows) + "</TABLE>>")


def build(config, fmt):
    dot = graphviz.Digraph(format=fmt)
    dot.attr(rankdir="TB", nodesep="1.0", ranksep="0.9", pad="0.5", dpi="160",
             bgcolor="white", label=f"<<B>{esc(config.get('game', ''))}</B>>",
             labelloc="t", fontsize="24", fontname=FONT, fontcolor=INK)
    dot.attr("node", shape="plain", fontname=FONT)
    dot.attr("edge", fontname=FONT, fontsize="12", color=EVENT_COLOR,
             fontcolor=EVENT_COLOR, penwidth="1.6", arrowsize="0.9")

    states = config["states"]
    start = config.get("start")

    dot.node("__start", shape="circle", label="", width="0.2", style="filled",
             fillcolor=START_HEADER, color=START_HEADER)
    dot.edge("__start", start)

    for name, state in states.items():
        dot.node(name, label=state_label(name, state, name == start))

    for name, state in states.items():
        loops = 0
        for event, action in reactions(state).items():
            action = action or {}
            if action.get("goto"):
                dot.edge(name, action["goto"], label=f"  {event}  ")
            else:
                # alternate sides so several loops on one state do not overlap
                east = loops % 2 == 0
                dot.edge(name, name, label=f"  {event}  ",
                         tailport="ne" if east else "nw",
                         headport="se" if east else "sw",
                         color=LOOP_COLOR, fontcolor=LOOP_COLOR)
                loops += 1
        after = state.get("after")
        if after:
            dot.edge(name, after["goto"], label=f"  after {after['delay']}  ",
                     style="dashed", color=TIMER_COLOR, fontcolor=TIMER_COLOR)

    dot.node("__key", label=legend_label())
    return dot


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("config", nargs="?", default=DEFAULT_CONFIG)
    ap.add_argument("-o", "--output", help="image path (default: next to the config)")
    ap.add_argument("-f", "--format", choices=["png", "svg", "pdf"], default=None)
    args = ap.parse_args()

    out = args.output or os.path.splitext(args.config)[0] + "." + (args.format or "png")
    fmt = args.format or os.path.splitext(out)[1].lstrip(".") or "png"

    with open(args.config) as f:
        config = yaml.safe_load(f)

    dot = build(config, fmt)
    try:
        dot.render(outfile=out, cleanup=True)
    except graphviz.ExecutableNotFound:
        sys.stderr.write("graphviz `dot` not found; run: brew install graphviz\n")
        sys.exit(2)
    print(out)


if __name__ == "__main__":
    main()
