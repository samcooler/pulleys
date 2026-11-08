"""Small dependency-free progress display for fw's HTTP and OTA transfers."""
import os
import shutil
import sys
import unicodedata


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
        self.last = None

    def __enter__(self):
        if self.interactive:
            self.stream.write("\x1b[?25l")
            self.stream.flush()
        return self

    def draw(self, title, items, note=""):
        if not self.enabled:
            return
        self.last = (title, items, note)
        if not self.interactive:
            self._log(title, items, note)
            return
        size = shutil.get_terminal_size(fallback=(80, 24))
        width, height = max(1, size.columns - 1), max(1, size.lines - 2)
        rows = list(items.items())
        # Keep active work and failures visible when the fleet exceeds the screen.
        priority = {"failed": 1, "uploading": 0, "validating": 0, "offering": 0, "connecting": 0,
                    "transferring": 0, "verifying": 0, "queued": 3}
        rows.sort(key=lambda row: (priority.get(row[1].get("state"), 2), row[0]))
        limit = max(0, height - 2)
        hidden = max(0, len(rows) - limit)
        lines = [fit(title, width)]
        lines += [device_line(node, item, width) for node, item in rows[:limit]]
        detail = note or next((f"{node}: {item.get('error') or item.get('message')}" for node, item in rows
                               if item.get("error") or item.get("message")), "")
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

    def _log(self, title, items, note="", *, final=False):
        title = clean(title)
        if title != self.log_title:
            print(title, file=self.stream, flush=True)
            self.log_title = title
        for node, item in items.items():
            value = percent(item)
            mark = (item.get("state"), item.get("attempt"), value // 10 if value is not None else None,
                    item.get("error"), item.get("message"))
            if final or self.log_rows.get(node) != mark:
                print("  " + device_line(node, item, 78), file=self.stream, flush=True)
                detail = item.get("error") or item.get("message")
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
