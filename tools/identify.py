#!/usr/bin/env python3
"""Ask every connected board what it is, over serial.

Each role answers "?" with one PULLEYS-ID line (see lib/pulleys_whoami). This
prints one TSV row per port so flash_all.sh can decide what to write where:

    port \t class \t role \t env \t id \t label \t name \t mac \t source

`source` is how the answer was obtained:
    id      the board replied to "?" -- role and env are what it is RUNNING
    boot    caught its boot-time PULLEYS-ID line
    silent  opened fine but said nothing (blank, crashed, or non-Pulleys
            firmware) -- class/role/env come back "?" and the caller should
            fall back to an esptool hardware probe
    error   port could not be opened

Usage: identify.py [port ...]     (default: all /dev/cu.usbmodem* + usbserial*)
"""

import glob
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial missing; run with ~/.platformio/penv/bin/python\n")
    sys.exit(2)

PREFIX = "PULLEYS-ID"
BAUD = 115200
# A board whose USB is native CDC reboots when the port opens, so the probe has
# to outlast a boot before concluding silence. The WROOM's CH340 does not reset
# (DTR/RTS held low) and answers on the first "?".
TIMEOUT = 6.0
UNKNOWN = "?"


def parse(line):
    """Pull the key=value fields out of a PULLEYS-ID line."""
    fields = {}
    for tok in line.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            fields[k] = v
    return fields


def probe(port, out):
    row = dict(port=port, cls=UNKNOWN, role=UNKNOWN, env=UNKNOWN,
               id=UNKNOWN, label=UNKNOWN, name=UNKNOWN, mac=UNKNOWN,
               source="silent")
    s = None
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = BAUD
        s.timeout = 0.2
        # The CH340 boards print nothing while DTR/RTS are asserted -- they sit
        # in reset. Deassert before and after open.
        s.dtr = False
        s.rts = False
        s.open()
        s.dtr = False
        s.rts = False
        s.reset_input_buffer()

        deadline = time.time() + TIMEOUT
        buf = b""
        asked = 0.0
        while time.time() < deadline:
            # Re-ask periodically: a board still booting will miss the first
            # request, and the reply is one line so extra asks are cheap.
            if time.time() - asked > 0.7:
                try:
                    s.write(b"?\n")
                    s.flush()
                except Exception:
                    pass
                asked = time.time()

            chunk = s.read(512)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if PREFIX not in text:
                    continue
                f = parse(text[text.index(PREFIX):])
                row.update(cls=f.get("class", UNKNOWN), role=f.get("role", UNKNOWN),
                           env=f.get("env", UNKNOWN), id=f.get("id", UNKNOWN),
                           label=f.get("label", UNKNOWN), name=f.get("name", UNKNOWN),
                           mac=f.get("mac", UNKNOWN), source="id")
                out.append(row)
                return
    except Exception:
        row["source"] = "error"
    finally:
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
    out.append(row)


def main():
    ports = sys.argv[1:]
    if not ports:
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    if not ports:
        return 1

    results, threads = [], []
    for p in ports:
        # Probing costs a full boot window, so all ports go at once.
        t = threading.Thread(target=probe, args=(p, results))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    order = {p: i for i, p in enumerate(ports)}
    for r in sorted(results, key=lambda r: order.get(r["port"], 0)):
        print("\t".join([r["port"], r["cls"], r["role"], r["env"],
                         r["id"], r["label"], r["name"], r["mac"], r["source"]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
