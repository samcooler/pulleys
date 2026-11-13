"""Small dependency-free progress display for fw's HTTP and OTA transfers."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
from collections import deque


def clean(value):
    # Status/error text comes back from the network; never interpret its escapes.
    return " ".join("".join(c if c.isprintable() else " " for c in str(value)).split())


def cells(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def fit(value, width):
    text = clean(value)
    if cells(text) <= width:
        return text
    out, used = "", 0
    for char in text:
        size = cells(char)
        if used + size > max(0, width - 1):
            break
        out += char
        used += size
    return out + ("~" if width else "")


def percent(item):
    if item.get("state") == "skipped":
        return None  # already current; no bytes were transferred
    if item.get("state") in {"confirmed", "uploaded"}:
        return 100
    if item.get("total", 0) > 0:
        return max(0, min(100, int(100 * item.get("bytes", 0) / item["total"])))
    return 0 if item.get("state") in {"queued", "offering", "connecting"} else None


def device_line(node, item, width):
    stage = clean(item.get("state", "queued"))
    if item.get("attempt"):
        stage += f" (try {item['attempt']}/3)"
    label = f"{clean(node):<6} {stage}"
    value = percent(item)
    right = f"{value:3d}%" if value is not None else " --%"
    if width < 12:
        return fit(label, max(0, width - 5)) + " " + right if width >= 5 else right[-width:] if width else ""
    bar_width = min(24, max(1, (width - 8) // 3))
    label_width = width - bar_width - 8
    text = fit(label, label_width)
    filled = int(bar_width * value / 100) if value is not None else 0
    bar = "#" * filled + "-" * (bar_width - filled)
    return text + " " * (label_width - cells(text)) + f" [{bar}] {right}"


def rate_text(bytes_per_second):
    kb = bytes_per_second / 1024
    return f"{kb / 1024:.1f} MB/s" if kb >= 1024 else f"{kb:.0f} KB/s" if kb >= 100 else f"{kb:.1f} KB/s"


class SpeedTracker:
    """Transfer speed per device, from the byte counts as they are polled: the rate
    over the last few seconds while transferring, then the average for the transfer
    once it is over."""
    WINDOW = 8.0

    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.samples = {}   # node -> deque of (time, bytes)
        self.first = {}     # node -> (time, bytes) of the first byte seen
        self.last = {}      # node -> (time, bytes) of the latest

    def text(self, node, item):
        now = self.clock()
        state, sent = item.get("state"), item.get("bytes", 0)
        if state == "transferring" and sent > 0:
            window = self.samples.setdefault(node, deque())
            if window and sent < window[-1][1]:      # a retry started over
                window.clear()
                self.first.pop(node, None)
            if not window or sent != window[-1][1] or now - window[-1][0] > 1:
                window.append((now, sent))
            while len(window) > 1 and now - window[0][0] > self.WINDOW:
                window.popleft()
            self.first.setdefault(node, (now, sent))
            self.last[node] = (now, sent)
            if len(window) >= 2 and window[-1][0] > window[0][0]:
                return rate_text((window[-1][1] - window[0][1]) / (window[-1][0] - window[0][0]))
            return ""
        if state in ("verifying", "confirmed") and node in self.first:
            (t0, b0), (t1, b1) = self.first[node], self.last[node]
            if t1 > t0 and b1 > b0:
                return "avg " + rate_text((b1 - b0) / (t1 - t0))
        return ""


FLASH_RIGHT_WIDTH = 20   # "100%  avg 999.9 MB/s"
STATUS_TEXT = {"queued": "Queued", "offering": "Offering...", "connecting": "Connecting...",
               "transferring": "Transferring", "verifying": "Verifying...", "confirmed": "Done",
               "skipped": "Already current", "cancelled": "Cancelled"}


def status_text(item):
    """The inline status for a device: what it is doing now, in a word or two."""
    state = item.get("state", "queued")
    if state == "failed":
        return "FAILED" + (f": {clean(item['error'])}" if item.get("error") else "")
    if state == "queued" and "paused" in str(item.get("message", "")):
        return "Held (queue paused)"
    return STATUS_TEXT.get(state, clean(state))


def flash_line(node, item, width):  # noqa: E302 (shows_detail is set below)
    """One device being flashed: ID, its firmware type, status, a bar, percent and speed."""
    stage = status_text(item)
    if item.get("attempt", 1) > 1 and item.get("state") not in ("confirmed", "skipped", "failed", "cancelled"):
        stage += f" (try {item['attempt']}/3)"
    value = percent(item)
    right = (f"{value:3d}%" if value is not None else " --%") + (f"  {clean(item['speed'])}" if item.get("speed") else "")
    if width < 40:
        return fit(" ".join(part for part in (clean(node), stage) if part), max(0, width - len(right) - 1)) + " " + right
    # Percent and speed sit in a column of fixed width, so every row's bar lines up.
    right = right.ljust(FLASH_RIGHT_WIDTH)
    bar_width = min(24, max(1, (width - len(right)) // 3))
    label_width = max(1, width - bar_width - len(right) - 4)
    # If it does not all fit, the firmware type gives way first: the status must stay readable.
    kind = clean(item.get("env", ""))
    room = label_width - cells(clean(node)) - cells(stage) - 2
    if kind and cells(kind) > room:
        kind = fit(kind, room) if room >= 4 else ""
    text = fit(" ".join(part for part in (clean(node), kind, stage) if part), label_width)
    filled = int(bar_width * value / 100) if value is not None else 0
    return (text + " " * (label_width - cells(text)) + f" [{'#' * filled}{'-' * (bar_width - filled)}] {right}").rstrip()


flash_line.shows_detail = True   # its status already says what went wrong, so no detail line under it


BUILD_MARKS = {"queued": "[--]", "building": "[..]", "built": "[ok]", "failed": "[!!]"}


def build_line(env, item, width):
    """One firmware type being compiled: its state on the left, how long on the right."""
    state = clean(item.get("state", "queued"))
    seconds = item.get("seconds")
    right = f"{state} {seconds}s" if seconds is not None else state
    label = f"{BUILD_MARKS.get(state, '[??]')} {clean(env)}"
    if width <= len(right) + 2:
        return fit(label, width)
    text = fit(label, width - len(right) - 1)
    return text + " " * (width - cells(text) - len(right)) + right


ACTIVITY_LINES = 4   # how many of the Overlord's recent messages sit under the bars


class ProgressDisplay:
    def __init__(self, *, enabled=True, stream=None):
        self.stream = stream if stream is not None else sys.stdout
        self.enabled = enabled
        self.interactive = enabled and self.stream.isatty() and os.environ.get("TERM") != "dumb"
        self.height = 0
        self.size = None
        self.previous = None
        self.log_title = None
        self.log_rows = {}
        self.log_note = None
        self.log_activity = set()
        self.last = None

    def __enter__(self):
        if self.interactive:
            self.stream.write("\x1b[?25l")
            self.stream.flush()
        return self

    def draw(self, title, items, note="", activity=(), render=None):
        """activity: (key, text) pairs, oldest first -- what the Overlord has been
        saying about this job. The newest few are shown under the device bars;
        plain output prints each one once, by key."""
        if not self.enabled:
            return
        ordered = render is not None   # a custom row keeps the order it was given
        render = render or device_line
        self.last = (title, items, note)
        if not self.interactive:
            self._log(title, items, note, render=render)
            for key, text in activity:
                if key not in self.log_activity:
                    self.log_activity.add(key)
                    print("  · " + clean(text), file=self.stream, flush=True)
            return
        activity = list(activity)[-ACTIVITY_LINES:]
        size = shutil.get_terminal_size(fallback=(80, 24))
        width, height = max(1, size.columns - 1), max(1, size.lines - 2)
        rows = list(items.items())
        # Keep active work and failures visible when the fleet exceeds the screen.
        priority = {"failed": 1, "uploading": 0, "validating": 0, "offering": 0, "connecting": 0,
                    "transferring": 0, "verifying": 0, "queued": 3}
        if not ordered:
            rows.sort(key=lambda row: (priority.get(row[1].get("state"), 2), row[0]))
        limit = max(0, height - 2 - len(activity))
        hidden = max(0, len(rows) - limit)
        lines = [fit(title, width)]
        lines += [render(node, item, width) for node, item in rows[:limit]]
        lines += ["  · " + fit(text, max(0, width - 4)) for _, text in activity]
        detail = note or ("" if getattr(render, "shows_detail", False) else
                          next((f"{node}: {item.get('error') or item.get('message')}" for node, item in rows
                                if item.get("error") or item.get("message")), ""))
        if hidden:
            detail = f"{hidden} more node(s); " + detail
        if height > 1:
            lines.append(fit(detail, width))
        frame = tuple(lines)
        if frame == self.previous and size == self.size:
            return
        if self.size is not None and size != self.size:
            # Resizing can reflow old lines. Start a fresh visible region, while
            # preserving scrollback (no erase-scrollback sequence).
            self.stream.write("\x1b[2J\x1b[H")
        elif self.height:
            self.stream.write(f"\x1b[{self.height}F")
        self.stream.write("\x1b[J" + "\n".join(lines) + "\n")
        self.stream.flush()
        self.height, self.size, self.previous = len(lines), size, frame

    def _log(self, title, items, note="", *, final=False, render=None):
        render = render or device_line
        title = clean(title)
        if title != self.log_title:
            print(title, file=self.stream, flush=True)
            self.log_title = title
        for node, item in items.items():
            value = percent(item)
            mark = (item.get("state"), item.get("attempt"), value // 10 if value is not None else None,
                    item.get("error"), item.get("message"))
            if final or self.log_rows.get(node) != mark:
                print("  " + render(node, item, 78), file=self.stream, flush=True)
                detail = None if getattr(render, "shows_detail", False) else item.get("error") or item.get("message")
                if detail:
                    print("    " + clean(detail), file=self.stream, flush=True)
                self.log_rows[node] = mark
        if note and note != self.log_note:
            print("  " + clean(note), file=self.stream, flush=True)
            self.log_note = note

    def __exit__(self, *_):
        if self.interactive:
            try:
                self.stream.write("\x1b[?25h")
                self.stream.flush()
            except (OSError, ValueError):
                pass
            # Emit off-screen failures once so their explanations remain in the
            # scrollback after a large job. Successful live rows stay in place.
            if self.last:
                failures = {node: item for node, item in self.last[1].items() if item.get("error")}
                if failures:
                    self._log("Device errors:", failures, final=True)


class BuildProgress:
    """A line per firmware type while PlatformIO compiles them one after another,
    updated as each finishes. The compiler's output is captured and printed only
    for a build that fails. Handed to release_firmware.build_bundle(progress=...)."""

    def __init__(self, *, enabled=True, stream=None, tick=0.5):
        self.display = ProgressDisplay(enabled=enabled, stream=stream)
        self.error_stream = sys.stderr
        self.items = {}
        self.tick = tick

    def __enter__(self):
        self.display.__enter__()
        return self

    def __exit__(self, *exc):
        return self.display.__exit__(*exc)

    def begin(self, envs):
        self.items = {env: {"state": "queued"} for env in envs}
        self._draw()

    def _draw(self):
        built = sum(item["state"] == "built" for item in self.items.values())
        failed = sum(item["state"] == "failed" for item in self.items.values())
        title = f"Building firmware | {built}/{len(self.items)} built"
        if failed:
            title += f" | {failed} failed"
        self.display.draw(title, self.items, "Compiler output is hidden unless a build fails (fw -v shows it).",
                          render=build_line)

    def run(self, env, command, cwd, child_env):
        item = self.items[env]
        started = time.monotonic()
        item.update(state="building", seconds=0)
        self._draw()
        with tempfile.TemporaryFile() as captured:
            process = subprocess.Popen(command, cwd=cwd, env=child_env, stdout=captured, stderr=subprocess.STDOUT)
            try:
                while True:
                    try:
                        code = process.wait(timeout=self.tick)
                        break
                    except subprocess.TimeoutExpired:
                        item["seconds"] = int(time.monotonic() - started)
                        self._draw()
            except BaseException:
                process.terminate()
                process.wait()
                raise
            captured.seek(0)
            output = captured.read().decode("utf-8", "replace")
        item.update(state="built" if code == 0 else "failed", seconds=int(time.monotonic() - started))
        self._draw()
        if code != 0:
            print(f"\n--- {env}: compiler output ---\n{output.rstrip()}\n--- end of {env} ---", file=self.error_stream, flush=True)
            raise RuntimeError(f"{env}: build failed (exit {code}); its compiler output is above")
