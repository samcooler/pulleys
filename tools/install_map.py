#!/usr/bin/env python3
"""Own the board tables in lib/pulleys_install/pulleys_install.h.

Two tables, keyed the same way — by a board's MAC-derived device ID:

    channels   which rope a Sensor drives
    displays   which display a Screen runs

Both have to survive being edited mid-install by someone distracted, so this
tool writes them: editing a C array in a hurry is how two ropes end up on one
channel. It is deliberately additive — it never changes a row that already
exists, so a value set by hand at a desk outlives every later run.

    install_map.py --list [--kind K]          print a table as TSV
    install_map.py --add ID [ID ...]          add missing IDs to the channel
                                              table, least-used value first
    install_map.py --add --kind display ID…   same, for the display table
    install_map.py --check ID [ID ...]        exit 1 if any ID is missing
    --kind channel|display    which table (default: channel)
    --dry-run                 say what would change, write nothing
    --file PATH               override the header location

An ID may be written 0xA855, A855, or N-A855, and may carry a name for the
generated comment: A855=N-A855.

Adding is idempotent: running it twice adds nothing the second time, so a
provisioning pass can be repeated safely after a board is swapped or a flash
fails halfway.
"""

import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HEADER = os.path.join(HERE, os.pardir, "lib", "pulleys_install",
                              "pulleys_install.h")

# Per-table settings. `lo`/`hi` bound the value; `names` is how a value is shown
# and how the tool talks about it. The channel range is kept in step with
# CHANNEL_FALLBACK_LO/HI in the header -- channel 0 is excluded on purpose, see
# the comment there. Displays are dense from 0 and named after ScreenDisplay.
KINDS = {
    "channel": dict(begin="// ASSIGNMENTS BEGIN", end="// ASSIGNMENTS END",
                    lo=1, hi=12, names=None, prefix="N"),
    "display": dict(begin="// DISPLAYS BEGIN", end="// DISPLAYS END",
                    lo=0, hi=1, names=["counter", "ranking"], prefix="X"),
}

ROW_RE = re.compile(r"\{\s*0x([0-9A-Fa-f]{1,4})\s*,\s*(\d+)\s*\}")


def parse_id(text):
    """Accept 0xA855, A855, N-A855, or any of those with =NAME appended."""
    name = None
    if "=" in text:
        text, name = text.split("=", 1)
    tok = text.strip()
    if "-" in tok:                      # N-A855 -> A855
        tok = tok.rsplit("-", 1)[1]
    tok = tok.lower()
    if tok.startswith("0x"):
        tok = tok[2:]
    if not tok or not all(c in "0123456789abcdef" for c in tok):
        raise ValueError("not a device ID: %r" % text)
    return int(tok, 16), name


class Table(object):
    def __init__(self, path, kind="channel"):
        self.kind = kind
        spec = KINDS[kind]
        self.BEGIN, self.END = spec["begin"], spec["end"]
        self.lo, self.hi = spec["lo"], spec["hi"]
        self.names, self.prefix = spec["names"], spec["prefix"]
        self.path = path
        with open(path, "r", encoding="utf-8") as fh:
            self.text = fh.read()
        try:
            self.head_end = self.text.index(self.BEGIN) + len(self.BEGIN)
            self.tail_start = self.text.index(self.END, self.head_end)
        except ValueError:
            raise SystemExit(
                "%s: could not find the '%s' / '%s' markers.\n"
                "The table is managed between those two lines; if they were "
                "removed, restore them inside CHANNEL_ASSIGNMENT[]."
                % (path, self.BEGIN, self.END))
        # Everything between the markers, minus the END line's indentation.
        block = self.text[self.head_end:self.tail_start]
        self.rows = []                  # list of (id, channel, name|None)
        for line in block.splitlines():
            m = ROW_RE.search(line)
            if not m:
                continue
            # Keep the whole trailing comment, not just the name token. Hand
            # edits are a first-class way into this table, and "// N-EC52 west
            # rope" is exactly the note someone leaves at a desk; silently
            # truncating it on the next run would punish the careful path.
            cm = re.search(r"//\s*(.*?)\s*$", line[m.end():])
            note = cm.group(1) if cm and cm.group(1) else None
            self.rows.append((int(m.group(1), 16), int(m.group(2)), note))

    def by_id(self):
        return {r[0]: r for r in self.rows}

    def next_free(self):
        """Lowest unused value, or the least-used one once they are all taken.

        Sharing is legitimate for both tables: several ropes on one channel
        report as one rope, which is a thing you might want of a cluster, and
        two screens can perfectly well show the same display. So running out of
        distinct values is not an error -- it just means new boards start
        doubling up, spread as evenly as the existing rows allow.
        """
        counts = {v: 0 for v in range(self.lo, self.hi + 1)}
        for r in self.rows:
            if r[1] in counts:
                counts[r[1]] += 1
        return min(sorted(counts), key=lambda v: counts[v])

    def add(self, wanted):
        """wanted: list of (id, name). Returns list of (id, channel, name)."""
        listed = self.by_id()
        added = []
        for dev_id, name in wanted:
            if dev_id in listed:
                continue
            row = (dev_id, self.next_free(), name)
            self.rows.append(row)
            listed[dev_id] = row
            added.append(row)
        return added

    def render(self):
        if not self.rows:
            return "\n"
        out = ["\n"]
        for dev_id, chan, name in sorted(self.rows, key=lambda r: r[1]):
            label = name or "%s-%04X" % (self.prefix, dev_id)
            line = "    { 0x%04X, %2d },   // %s" % (dev_id, chan, label)
            out.append(line.rstrip() + "\n")
        out.append("    ")
        return "".join(out)

    def write(self):
        new = self.text[:self.head_end] + self.render() + self.text[self.tail_start:]
        if new == self.text:
            return False
        # Write via a temp file in the same directory then rename, so an
        # interrupted run cannot leave a half-written header that will not
        # compile -- the whole point of this tool is being trustworthy when
        # someone is in a hurry.
        d = os.path.dirname(os.path.abspath(self.path))
        fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                fh.write(new)
            os.replace(tmp, self.path)
        except BaseException:
            if os.path.exists(tmp):
                os.unlink(tmp)
            raise
        self.text = new
        return True


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=DEFAULT_HEADER)
    ap.add_argument("--kind", choices=sorted(KINDS), default="channel")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--add", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("ids", nargs="*")
    args = ap.parse_args()

    table = Table(args.file, args.kind)

    if args.list:
        for dev_id, val, name in sorted(table.rows, key=lambda r: r[1]):
            shown = table.names[val] if table.names and val < len(table.names) else val
            print("%04X\t%s\t%s" % (dev_id, shown, name or ""))
        return 0

    try:
        wanted = [parse_id(t) for t in args.ids]
    except ValueError as e:
        sys.stderr.write("%s\n" % e)
        return 2

    if args.check:
        listed = table.by_id()
        missing = [i for i, _ in wanted if i not in listed]
        for i in missing:
            print("missing\t%04X" % i)
        return 1 if missing else 0

    if not args.add:
        ap.error("nothing to do: pass --list, --check, or --add")
    if not wanted:
        ap.error("--add needs at least one device ID")

    added = table.add(wanted)
    if not added:
        print("%s table unchanged - all %d board(s) already listed."
              % (table.kind.capitalize(), len(wanted)))
        return 0
    for dev_id, val, name in added:
        shown = table.names[val] if table.names and val < len(table.names) else val
        print("  + 0x%04X -> %s %s%s" % (dev_id, table.kind, shown,
                                         "  (%s)" % name if name else ""))
    if args.dry_run:
        print("Dry run — %s not written." % args.file)
        return 0
    table.write()
    print("Wrote %d new assignment(s) to %s" % (len(added), args.file))
    return 0


if __name__ == "__main__":
    sys.exit(main())
