#!/usr/bin/env python3
"""Ask the whole piece what it is, over the mesh.

Plug into any one board and it broadcasts a HELLO; every board on the mesh
answers with its role, the env and build it is running, a role-specific detail
and its uptime. Nothing is interrupted -- boards answer from inside their normal
loop and carry on with the art -- and no network of any kind is involved.

    survey.py [-p <port>] [--spread <ms>] [--json]

This is the wireless counterpart to `./flash_all.sh -l`, which can only see what
is plugged in. Use it to answer "what is out there, and what is it running?",
especially straight after an over-the-air round.
"""

import argparse
import glob
import json as jsonlib
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial missing; run with ~/.platformio/penv/bin/python\n")
    sys.exit(2)

BAUD = 115200
PREFIX = "PULLEYS-CENSUS"
DEFAULT_SPREAD_MS = 900
# Listen past the reply window: a board answering at the far edge of it still
# has to get its packet across, and a late answer beats a missing row.
LISTEN_SLACK = 2.5

# detail="..." is quoted so an empty one still parses; everything else is a
# bare key=value token.
FIELD_RE = re.compile(r'(\w+)=(?:"([^"]*)"|(\S+))')


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 0.3
    # Same reason as identify.py: the CH340 boards sit in reset while DTR/RTS
    # are asserted and print nothing at all.
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def find_port(explicit):
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    if not ports:
        sys.exit("No board on USB. Plug into any one of them — it does the asking.")
    return ports[0]


def parse(line):
    out = {}
    for k, quoted, bare in FIELD_RE.findall(line):
        out[k] = quoted if bare == "" else bare
    return out


def collect(port, spread_ms):
    """Run one census round and return the rows, newest answer per board."""
    s = open_port(port)
    try:
        s.write(f"CENSUS {spread_ms}\n".encode())
        s.flush()
        rows = {}
        deadline = time.time() + (spread_ms / 1000.0) + LISTEN_SLACK
        while time.time() < deadline:
            line = s.readline().decode("utf-8", "replace").strip()
            if not line or PREFIX not in line:
                continue
            body = line[line.index(PREFIX):]
            if " id=" not in body:
                continue          # the "begin" line
            f = parse(body)
            if f.get("id"):
                rows[f["id"]] = f
        return rows
    finally:
        s.close()


def render(rows):
    if not rows:
        print("Nothing answered. Are the boards powered and on the mesh channel?")
        return

    def sort_key(f):
        # Sensors first and in rope order, since that is how the piece is read.
        role = f.get("role", "")
        rank = {"SENSOR": 0, "SCREEN": 1, "ARBITER": 2, "BRIDGE": 3}.get(role, 4)
        m = re.match(r"ch(\d+)", f.get("detail", ""))
        return (rank, int(m.group(1)) if m else 99, f.get("id", ""))

    items = sorted(rows.values(), key=sort_key)
    builds = {f.get("build", "?") for f in items}

    w = {"name": 8, "role": 7, "env": 12, "detail": 12, "build": 21}
    print(f"{'BOARD':<{w['name']}} {'ROLE':<{w['role']}} {'ENV':<{w['env']}} "
          f"{'DETAIL':<{w['detail']}} {'BUILD':<{w['build']}} UP")
    for f in items:
        up = int(f.get("up", 0))
        uptime = f"{up // 3600}h{(up % 3600) // 60:02d}m" if up >= 3600 else f"{up // 60}m{up % 60:02d}s"
        here = " ←usb" if f.get("self") == "1" else ""
        print(f"{f.get('name','?'):<{w['name']}} {f.get('role','?'):<{w['role']}} "
              f"{f.get('env','?'):<{w['env']}} {f.get('detail',''):<{w['detail']}} "
              f"{f.get('build','?'):<{w['build']}} {uptime}{here}")

    print(f"\n{len(items)} board(s).", end=" ")
    if len(builds) == 1:
        print("All on the same build.")
    else:
        # The thing you actually want to know after an over-the-air round.
        print(f"{len(builds)} different builds — these are out of step:")
        for b in sorted(builds):
            who = " ".join(f.get("name", "?") for f in items if f.get("build") == b)
            print(f"  {b:<21} {who}")


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("-p", dest="port", default=None)
    ap.add_argument("--spread", type=int, default=DEFAULT_SPREAD_MS,
                    help="ms for boards to spread their answers over")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    rows = collect(find_port(a.port), a.spread)
    if a.json:
        print(jsonlib.dumps(sorted(rows.values(), key=lambda f: f.get("id", "")), indent=2))
    else:
        render(rows)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
