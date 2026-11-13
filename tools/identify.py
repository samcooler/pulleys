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
FAST_BAUD = 921600
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


def probe(port, out, versions=False):
    """Identify one port; optionally collect its own census build identity.

    The legacy TSV output remains unchanged. Callers requesting versions get
    extra build/sha256 fields in the returned dictionary.
    """
    row = dict(port=port, cls=UNKNOWN, role=UNKNOWN, env=UNKNOWN,
               id=UNKNOWN, label=UNKNOWN, name=UNKNOWN, mac=UNKNOWN,
               source="silent", build=UNKNOWN, sha256="unknown")
    s = None
    identified = False
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = BAUD
        s.timeout = 0.2
        s.write_timeout = 1.0   # never tcdrain: it has no timeout and hangs on a CDC port that is not draining
        s.dtr = False
        s.rts = False
        s.open()
        s.dtr = False
        s.rts = False
        s.reset_input_buffer()
        # Keep ordinary nodes at 115200; fast bridge firmware boots at 921600.
        for baud in (BAUD, FAST_BAUD):
            s.baudrate = baud
            s.reset_input_buffer()
            deadline = time.monotonic() + (TIMEOUT if baud == BAUD else 2.0)
            version_deadline = None
            buf = b""
            identified = False
            asked = 0.0
            while time.monotonic() < (version_deadline or deadline):
                if not identified and time.monotonic() - asked > 0.7:
                    s.write(b"?\n")
                    asked = time.monotonic()
                chunk = s.read(512)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if PREFIX in line:
                        fields = parse(line[line.index(PREFIX):])
                        row.update(cls=fields.get("class", UNKNOWN), role=fields.get("role", UNKNOWN),
                                   env=fields.get("env", UNKNOWN), id=fields.get("id", UNKNOWN),
                                   label=fields.get("label", UNKNOWN), name=fields.get("name", UNKNOWN),
                                   mac=fields.get("mac", UNKNOWN), build=fields.get("build", UNKNOWN),
                                   sha256=fields.get("sha256", "unknown"), source="id", baud=baud)
                        identified = True
                        if not versions or len(row["sha256"]) == 64:
                            out.append(row)
                            return
                        if version_deadline is None:
                            s.write(b"CENSUS 500\n")
                            version_deadline = time.monotonic() + 1.5
                    elif identified and "PULLEYS-CENSUS " in line:
                        fields = parse(line[line.index("PULLEYS-CENSUS "):])
                        if fields.get("id", "").upper() == row["id"].upper() and fields.get("self") == "1":
                            row["sha256"] = fields.get("sha256", "unknown")
                            out.append(row)
                            return
            if identified:
                out.append(row)
                return
    except Exception as exc:
        row["source"] = "error"
        row["error"] = str(exc)
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
