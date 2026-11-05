#!/usr/bin/env python3
"""Write a game state config as a readable walkthrough, one HTML page.

    state_doc.py [config.yaml] [-o out.html] [--fragment] [--png]

Reads game_state_machine/state_config.yaml by default and writes
state_config.html next to it. Needs only pyyaml. --png also writes a full-page
screenshot next to it, with every wildcard expanded; that needs Playwright
(pip3 install playwright && python3 -m playwright install chromium).

The game is close to a straight line, so the page is a walkthrough in order:
the start state first, then each state the previous one moves on to. A state
is a section with what plays on entry, a table of what each pull does, the
idle line and the timeout. A wildcard cue (video_once_janis_nag_*) is expanded
to the takes it can pick from, with the line Janis says in each, read from the
comments beside the cues in the YAML. `choose_one` lists show every option.

--fragment leaves out the document wrapper (doctype, html, head, body) for a
host that supplies its own.
"""

import argparse
import html
import os
import re
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write("missing deps; run: pip3 install pyyaml\n")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(ROOT, "game_state_machine", "state_config.yaml")

# What each event means, in the words of the state_config glossary.
EVENTS = {
    "first_goal_component_activated": "a goal lights while no goal is lit",
    "goal_component_activated": "a correct component is pulled",
    "nongoal_component_activated": "a component outside the goal set is pulled",
    "component_deactivated": "a major component goes dark",
    "all_components_activated": "every goal lit, every non-goal dark",
    "all_components_deactivated": "every major component dark",
    "final_component_activated": "a purple rope is pulled",
    "final_component_deactivated": "a purple component goes dark",
    "all_final_components_activated": "every purple rope lit",
}
PURPLE_EVENTS = {e for e in EVENTS if e.startswith("final_") or e.startswith("all_final")}


def esc(text):
    return html.escape(str(text), quote=True)


def reactions(state):
    return state.get("on") or state.get(True) or {}   # YAML 1.1 reads `on:` as True


def read_comments(path):
    """What the YAML's comments say that the parsed config cannot:
    lines: {cue: comment} for every `- cue  # comment` line (Janis's words);
    placeholders: {cue} for a cue line commented placeholder, and
    {(state, "idle"|"after")} for a delay commented placeholder."""
    lines, placeholders = {}, set()
    state, block = None, None
    with open(path) as f:
        for raw in f:
            m = re.match(r"\s*-\s+([a-z0-9_*-]+)\s+#\s*(.+?)\s*$", raw)
            if m:
                lines[m.group(1)] = m.group(2)
                if "placeholder" in m.group(2).lower():
                    placeholders.add(m.group(1))
                continue
            m = re.match(r"  ([a-z0-9_]+):\s*$", raw)
            if m:
                state, block = m.group(1), None
            m = re.match(r"    (idle|after):\s*$", raw)
            if m:
                block = m.group(1)
            if "delay:" in raw and "placeholder" in raw.lower() and state and block:
                placeholders.add((state, block))
    return lines, placeholders


def order_states(config):
    """States in the order the game walks them: start, then each goto in turn."""
    states = config["states"]
    seen, order = set(), []

    def visit(name):
        if name in seen or name not in states:
            return
        seen.add(name)
        order.append(name)
        st = states[name] or {}
        nexts = [(a or {}).get("goto") for a in reactions(st).values()]
        if st.get("after"):
            nexts.append(st["after"].get("goto"))
        for n in nexts:
            if n:
                visit(n)

    visit(config["start"])
    for name in states:          # anything unreachable, at the end
        visit(name)
    return order


class Doc:
    def __init__(self, config, lines, placeholders=()):
        self.config = config
        self.lines = lines
        self.placeholders = set(placeholders)
        self.all_cues = [c for group in (config.get("cues") or {}).values() for c in group or []]
        self.states = config["states"]
        self.order = order_states(config)

    # -- cues ---------------------------------------------------------------
    def kind(self, cue):
        for k in ("video_loop", "video_once", "audio_loop", "audio_once", "game"):
            if cue.startswith(k + "_") or cue == "audio_loop_off":
                return "audio_loop" if cue == "audio_loop_off" else k
        return "other"

    def takes(self, pattern):
        """The listed cues a wildcard can pick from (excluding other wildcards)."""
        rx = re.compile("^" + ".*".join(re.escape(p) for p in pattern.split("*")) + "$")
        return [c for c in self.all_cues if "*" not in c and rx.match(c)]

    def is_placeholder(self, cue):
        """A cue still standing in for real media: commented so, or test media."""
        return cue in self.placeholders or "test" in cue.split("_")

    def ph(self, cue):
        return '<span class="ph">placeholder</span>' if self.is_placeholder(cue) else ""

    def delay_html(self, state, block, delay):
        flag = (state, block) in self.placeholders
        return f'<b class="{"ph-b" if flag else ""}">{esc(delay)}</b>' + ('<span class="ph">placeholder</span>' if flag else "")

    def cue_html(self, cue, enter=False):
        kind = self.kind(cue)
        label = {"video_loop": "video loop", "video_once": "clip", "audio_loop": "music",
                 "audio_once": "sound", "game": "game", "other": "cue"}[kind]
        name = esc(cue)
        if "*" in cue:
            takes = self.takes(cue)
            stem = cue.replace("*", "")
            items = "".join(
                f'<li><code>{esc(t[len(stem):] if t.startswith(stem) else t)}</code>'
                + (f' <span class="line">“{esc(self.lines[t])}”</span>' if t in self.lines and not self.is_placeholder(t) else "")
                + self.ph(t) + "</li>" for t in takes)
            return (f'<details class="cue k-{kind}"><summary><span class="tag">{label}</span>'
                    f'<code>{name}</code><span class="n">one of {len(takes)} takes</span></summary>'
                    f'<ul class="takes">{items}</ul></details>')
        line = f' <span class="line">“{esc(self.lines[cue])}”</span>' if cue in self.lines and not self.is_placeholder(cue) else ""
        return f'<div class="cue k-{kind}"><span class="tag">{label}</span><code>{name}</code>{line}{self.ph(cue)}</div>'

    def cues_html(self, cues, enter=False):
        if not cues:
            return '<span class="none">nothing</span>'
        parts = []
        for c in cues:
            if isinstance(c, dict):
                opts = "".join(self.cue_html(o) for o in c["choose_one"])
                parts.append(f'<div class="choose"><div class="choose-h">one of, at random</div>{opts}</div>')
            else:
                parts.append(self.cue_html(c, enter))
        return "".join(parts)

    # -- states ---------------------------------------------------------------
    def state_html(self, i, name):
        st = self.states[name] or {}
        on = reactions(st)
        enter = (on.get("enter") or {}).get("cue") or []
        rows = []
        for event, action in on.items():
            if event == "enter":
                continue
            action = action or {}
            cls = ' class="purple"' if event in PURPLE_EVENTS else ""
            goto = (f'<a class="goto" href="#{esc(action["goto"])}">→ {esc(action["goto"])}</a>'
                    if action.get("goto") else "")
            rows.append(f'<tr{cls}><td class="ev"><code>{esc(event)}</code>'
                        f'<div class="evd">{esc(EVENTS.get(event, ""))}</div></td>'
                        f'<td class="do">{self.cues_html(action.get("cue"))}{goto}</td></tr>')
        table = (f'<table class="react"><thead><tr><th>When</th><th>Janis and the panel</th></tr></thead>'
                 f'<tbody>{"".join(rows)}</tbody></table>' if rows else
                 '<p class="none">No pull does anything here.</p>')
        idle = st.get("idle")
        idle_html = (f'<div class="timer"><span class="tag t-idle">idle</span>'
                     f'<span>after {self.delay_html(name, "idle", idle["delay"])} without a pull, then every {esc(idle["delay"])}</span>'
                     f'<div class="timer-cues">{self.cues_html(idle.get("cue"))}</div></div>'
                     if idle else "")
        after = st.get("after")
        after_html = (f'<div class="timer"><span class="tag t-after">timeout</span>'
                      f'<span>after {self.delay_html(name, "after", after.get("delay", "2m"))} in this state, '
                      f'<a class="goto" href="#{esc(after["goto"])}">→ {esc(after["goto"])}</a></span></div>'
                      if after else "")
        start = ' <span class="start">start</span>' if name == self.config["start"] else ""
        return f'''
<section class="state" id="{esc(name)}">
  <header><span class="num">{i}</span><h2>{esc(name)}{start}</h2></header>
  <div class="block">
    <h3>On entry <span class="hint">these clips cut off whatever is playing</span></h3>
    <div class="cues">{self.cues_html(enter, enter=True)}</div>
  </div>
  <div class="block">
    <h3>Reactions <span class="hint">a clip here waits its turn: dropped if one is playing</span></h3>
    {table}
  </div>
  {f'<div class="block timers">{idle_html}{after_html}</div>' if idle or after else ""}
</section>'''

    def strip_html(self):
        pills = []
        for name in self.order:
            st = self.states[name] or {}
            after = st.get("after")
            ph = ' class="ph-b"' if (name, "after") in self.placeholders else ""
            tail = f'<small><span{ph}>{esc(after["delay"])}</span> → {esc(after["goto"])}</small>' if after else "<small>&nbsp;</small>"
            pills.append(f'<a href="#{esc(name)}"><b>{esc(name)}</b>{tail}</a>')
        return '<nav class="strip">' + "".join(pills) + "</nav>"

    def render(self, fragment=False):
        game = self.config.get("game", "game")
        sections = "".join(self.state_html(i + 1, n) for i, n in enumerate(self.order))
        ambient = self.config.get("ambient_sounds") or []
        amb = "".join(f'<li><code>{esc(a["sound"])}</code> every {esc(a["interval"])}s '
                      f'± {esc(a.get("variance", 0))}s</li>' for a in ambient)
        body = f'''<title>{esc(game)} flow</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* Layout: one column walkthrough, a strip of phases at the top, a section per phase. */
:root {{
  --bg: #f6f5f1; --paper: #ffffff; --fg: #23262b; --muted: #6b6f76; --rule: #dddad2;
  --accent: #0b6e6b; --accent-ink: #ffffff;
  --video: #1f4fb8; --video-bg: #e9eefb; --audio: #0e7a52; --audio-bg: #e6f4ec;
  --game: #7a5a11; --game-bg: #f7efd9; --purple: #6b32b5; --purple-bg: #f1eafb;
  --idle: #9a4f0a; --after: #4b5563; --warn: #c8102e; --warn-bg: #fde8eb;
  --sans: "IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, "SF Mono", Menlo, Consolas, monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  --bg: #16181c; --paper: #1f2227; --fg: #e8e6df; --muted: #9a9ea6; --rule: #343840;
  --accent: #3fb8b3; --accent-ink: #0d1413;
  --video: #8fb0ff; --video-bg: #22304f; --audio: #6fd3a3; --audio-bg: #1c3a2c;
  --game: #e3bd5c; --game-bg: #3b3119; --purple: #c39cff; --purple-bg: #30234a;
  --idle: #f0a55a; --after: #b2b7c0; --warn: #ff6b7a; --warn-bg: #4a1f26; color-scheme: dark }} }}
:root[data-theme="dark"] {{
  --bg: #16181c; --paper: #1f2227; --fg: #e8e6df; --muted: #9a9ea6; --rule: #343840;
  --accent: #3fb8b3; --accent-ink: #0d1413;
  --video: #8fb0ff; --video-bg: #22304f; --audio: #6fd3a3; --audio-bg: #1c3a2c;
  --game: #e3bd5c; --game-bg: #3b3119; --purple: #c39cff; --purple-bg: #30234a;
  --idle: #f0a55a; --after: #b2b7c0; --warn: #ff6b7a; --warn-bg: #4a1f26; color-scheme: dark }}
body {{ background: var(--bg); color: var(--fg); font: 15px/1.5 var(--sans); margin: 0;
  padding-block: 32px 64px;
  padding-left: max(16px, env(safe-area-inset-left, 0px));
  padding-right: max(16px, env(safe-area-inset-right, 0px)); }}
main {{ max-width: 860px; margin: 0 auto; display: grid; gap: 28px; }}
h1 {{ font-size: 28px; font-weight: 600; margin: 0; letter-spacing: -0.01em; text-wrap: balance; }}
.lede {{ color: var(--muted); max-width: 62ch; margin: 6px 0 0; }}
code {{ font-family: var(--mono); font-size: 0.92em; }}
.strip {{ display: flex; flex-wrap: wrap; gap: 6px; }}
.strip a {{ display: grid; gap: 1px; padding: 8px 12px; border: 1px solid var(--rule); border-radius: 6px;
  background: var(--paper); color: inherit; text-decoration: none; min-width: 0; }}
.strip a b {{ font-weight: 500; }}
.strip a small {{ color: var(--muted); font-size: 11px; }}
.strip a:hover, .strip a:focus-visible {{ border-color: var(--accent); outline: none; }}
.state {{ background: var(--paper); border: 1px solid var(--rule); border-radius: 8px; padding: 20px 22px; display: grid; gap: 18px; }}
.state header {{ display: flex; align-items: baseline; gap: 12px; }}
.num {{ font: 500 12px var(--mono); color: var(--accent-ink); background: var(--accent); border-radius: 999px;
  padding: 2px 8px; }}
h2 {{ margin: 0; font-size: 22px; font-weight: 600; }}
h2 code, h2 {{ font-family: var(--mono); }}
.start {{ font: 500 11px var(--sans); text-transform: uppercase; letter-spacing: 0.08em; color: var(--muted); margin-left: 8px; }}
.note {{ margin: -6px 0 0; color: var(--muted); max-width: 65ch; }}
h3 {{ margin: 0 0 8px; font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.08em; color: var(--muted); }}
.hint {{ font-weight: 400; text-transform: none; letter-spacing: 0; margin-left: 6px; }}
.cues, .do, .timer-cues {{ display: grid; gap: 6px; }}
.cue {{ display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px; min-width: 0; }}
.cue summary {{ display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px; cursor: pointer; list-style: none; }}
.cue summary::-webkit-details-marker {{ display: none; }}
.cue summary .n {{ color: var(--accent); font-size: 13px; }}
.cue summary .n::before {{ content: "▸ "; }}
.cue[open] summary .n::before {{ content: "▾ "; }}
.tag {{ font: 500 10.5px var(--sans); text-transform: uppercase; letter-spacing: 0.06em; padding: 1px 6px; border-radius: 4px; }}
.k-video_once .tag, .k-video_loop .tag {{ color: var(--video); background: var(--video-bg); }}
.k-audio_once .tag, .k-audio_loop .tag {{ color: var(--audio); background: var(--audio-bg); }}
.k-game .tag, .k-other .tag {{ color: var(--game); background: var(--game-bg); }}
.line {{ color: var(--muted); font-style: italic; }}
.takes {{ margin: 6px 0 2px; padding-left: 18px; display: grid; gap: 2px; font-size: 14px; }}
.choose {{ border-left: 2px solid var(--rule); padding-left: 10px; display: grid; gap: 4px; }}
.choose-h {{ font-size: 12px; color: var(--muted); }}
.react {{ width: 100%; border-collapse: collapse; }}
.react th {{ text-align: left; font-size: 12px; font-weight: 500; color: var(--muted); padding: 0 8px 6px 0; border-bottom: 1px solid var(--rule); }}
.react td {{ vertical-align: top; padding: 10px 8px 10px 0; border-bottom: 1px solid var(--rule); min-width: 0; }}
.react tr:last-child td {{ border-bottom: 0; }}
.react .ev {{ width: 38%; }}
.evd {{ color: var(--muted); font-size: 13px; }}
tr.purple .ev code {{ color: var(--purple); background: var(--purple-bg); padding: 0 4px; border-radius: 3px; }}
.goto {{ color: var(--accent); text-decoration: none; font-weight: 500; }}
.goto:hover {{ text-decoration: underline; }}
.none {{ color: var(--muted); margin: 0; }}
.timers {{ display: grid; gap: 10px; }}
.timer {{ display: grid; grid-template-columns: auto 1fr; gap: 6px 10px; align-items: baseline; }}
.timer-cues {{ grid-column: 2; }}
.t-idle {{ color: var(--idle); border: 1px solid currentColor; }}
.t-after {{ color: var(--after); border: 1px solid currentColor; }}
.ph {{ font: 500 10.5px var(--sans); text-transform: uppercase; letter-spacing: 0.06em; padding: 1px 6px; border-radius: 4px;
  color: var(--warn); background: var(--warn-bg); margin-left: 4px; }}
.ph-b {{ color: var(--warn); font-weight: 600; }}
.ambient {{ color: var(--muted); font-size: 14px; }}
.ambient ul {{ margin: 4px 0 0; padding-left: 18px; }}
@media (max-width: 560px) {{
  .react, .react thead, .react tbody, .react tr, .react td {{ display: block; }}
  .react thead {{ display: none; }}
  .react .ev {{ width: auto; padding-bottom: 4px; border-bottom: 0; }}
  .react td.do {{ padding-top: 0; }}
  .state {{ padding: 16px; }}
}}
@media (prefers-reduced-motion: no-preference) {{ html {{ scroll-behavior: smooth; }} }}
</style>
<main>
  <div>
    <h1>{esc(game)}</h1>
    <p class="lede">Every phase of the game in the order players meet it. Each one lists what the
    panel plays on entry, what each pull does, and when Janis speaks up on her own.
    <span class="ph">placeholder</span> marks media or timings still standing in for the real thing.</p>
  </div>
  {self.strip_html()}
  {sections}
  <div class="ambient">Always, whatever the phase: <ul>{amb}</ul></div>
</main>
'''
        if fragment:
            return body
        return ('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
                '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
                + body + "</head>\n</html>\n").replace("</style>\n<main>", "</style>\n</head>\n<body>\n<main>") \
            .replace("</main>\n</head>\n</html>", "</main>\n</body>\n</html>")


def write_png(doc, path):
    """Screenshot the walkthrough with every wildcard opened, 2x, light theme."""
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        sys.stderr.write("--png needs playwright: pip3 install playwright && "
                         "python3 -m playwright install chromium\n")
        sys.exit(2)
    page_html = doc.render().replace('<details class="cue', '<details open class="cue')
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1000, "height": 1200},
                                device_scale_factor=2, color_scheme="light")
        page.set_content(page_html)
        page.wait_for_timeout(1500)   # let the web fonts arrive
        page.screenshot(path=path, full_page=True)
        browser.close()
    print(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config", nargs="?", default=DEFAULT_CONFIG)
    ap.add_argument("-o", "--out", help="output path (default: next to the config)")
    ap.add_argument("--fragment", action="store_true", help="no document wrapper")
    ap.add_argument("--png", action="store_true", help="also screenshot the page (needs playwright)")
    args = ap.parse_args()
    with open(args.config) as f:
        config = yaml.safe_load(f)
    lines, placeholders = read_comments(args.config)
    doc = Doc(config, lines, placeholders)
    out = args.out or os.path.splitext(args.config)[0] + ".html"
    with open(out, "w") as f:
        f.write(doc.render(fragment=args.fragment))
    print(out)
    if args.png:
        write_png(doc, os.path.splitext(out)[0] + ".png")


if __name__ == "__main__":
    main()
