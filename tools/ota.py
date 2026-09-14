#!/usr/bin/env python3
"""Reflash the whole piece over the air, through one USB-tethered board.

Plug into any one board. That board becomes the OTA host: the image is written
into its spare flash slot with esptool -- the same wired path that flashes these
boards every day -- and it then serves that image on its own SoftAP, raised on
the mesh channel, while repeating an ESP-NOW invitation. Every board running the
named env joins, pulls, flashes itself and reboots.

Nothing here touches the network: no internet, no router, and your machine never
joins the piece's AP. It works on a floor with no infrastructure at all.

    ota.py                    every live env in turn, then the tethered board
                              itself over its cable. The whole piece, one command
    ota.py <env>              just that env
    ota.py <env> -i A855      just that one board

    -p <port>     serial port of the tethered board (default: the first found)
    --window <s>  how long each round waits for nodes (default 120)
    --tries <n>   attempts per env when a board reports a failure (default 3)
    --no-build    use the firmware.bin already in .pio/build/<env>
    --keep-host   do not reflash the tethered board at the end
    --no-survey   skip the before/after census of the mesh

Before pushing anything it surveys the mesh, so you can see what is out there
and what each board is running; afterwards it surveys again, which is how you
confirm the update actually landed.

The tethered board cannot update itself over its own AP, so the full run
finishes by flashing it over the cable with the env it already runs.
"""

import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial missing; run with ~/.platformio/penv/bin/python\n")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import survey  # noqa: E402  — sibling tool; the census parsing lives there
ROOT = os.path.dirname(HERE)
PY = os.path.expanduser("~/.platformio/penv/bin/python")
PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
FLASH_ALL = os.path.join(ROOT, "flash_all.sh")
BAUD = 115200
PREFIX = "PULLEYS-OTA"

# Mirrors flash_all.sh: the chip per hardware class, read from the board itself.
CHIP_FOR_CLASS = {"s3_4mb": "esp32s3", "s3_16mb": "esp32s3",
                  "esp32": "esp32", "c3": "esp32c3"}

# How long to wait for the first check-in before concluding that no board of
# this env is listening. An invited node joins within a few seconds, so this
# only has to outlast a join, not the whole round.
NO_NODE_GRACE = 30.0


def log(msg):
    print(msg, flush=True)


def live_envs():
    """The envs platformio.ini actually builds, so this stays in step with it."""
    txt = open(os.path.join(ROOT, "platformio.ini")).read()
    m = re.search(r"^default_envs\s*=\s*(.+)$", txt, re.M)
    if not m:
        sys.exit("No default_envs in platformio.ini — name an env explicitly.")
    return [e.strip() for e in m.group(1).split(",") if e.strip()]


# ── Serial ───────────────────────────────────────────────────────────────────

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


def fields_of(line):
    out = {}
    for tok in line.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def ask(s, command, want, timeout=8.0):
    """Send a line, return the fields of the first reply containing `want`."""
    s.write(command.encode() + b"\n")
    s.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if want in line:
            return fields_of(line[line.index(PREFIX):] if PREFIX in line else line)
    return None


def identify(port, timeout=10.0):
    """Ask one port what it is. Returns the PULLEYS-ID fields, or None."""
    try:
        s = open_port(port)
    except Exception:
        return None
    try:
        return ask(s, "?", "PULLEYS-ID", timeout)
    finally:
        s.close()


def resolve_port(dev_id, hint=None, timeout=30.0):
    """Find the port the board with this device ID is on, now.

    Every reset re-enumerates a native-USB board, and macOS does not promise
    the same /dev path afterwards -- the two boards on this bench swap between
    usbmodem101 and usbmodem2101 routinely. Addressing by device ID rather than
    by path is the difference between a run that survives its own reboots and
    one that flashes whatever happens to be sitting on that path.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        ports = []
        if hint:
            ports.append(hint)
        ports += sorted(p for p in glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
                        if p != hint)
        for p in ports:
            f = identify(p, timeout=6.0)
            if f and (dev_id is None or f.get("id", "").upper() == dev_id.upper()):
                return p, f
        time.sleep(0.5)
    return None, None


# ── Building and writing ─────────────────────────────────────────────────────

def build(env):
    log(f"  building {env}…")
    r = subprocess.run([PIO, "run", "-e", env], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-2000:])
        sys.exit(f"Build FAILED — run 'pio run -e {env}' to see why.")


def firmware_for(env):
    fw = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
    if not os.path.exists(fw):
        sys.exit(f"No firmware at {fw} — build {env} first.")
    return fw


def write_image(port, chip, offset, image_path):
    """esptool the image into the host's spare slot, resetting it afterwards."""
    r = subprocess.run(
        [PY, ESPTOOL, "--chip", chip, "--port", port, "--baud", "460800",
         "--before", "default_reset", "--after", "hard_reset",
         "write_flash", "-z", offset, image_path],
        capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write((r.stdout + r.stderr)[-1500:])
        return False
    for line in (r.stdout + r.stderr).splitlines():
        if "Wrote" in line:
            log("  " + line.strip())
    return True


# ── One serving round ────────────────────────────────────────────────────────

def watch(s, window):
    """Follow the host's lines until every node that answered has settled."""
    seen = {}
    deadline = time.time() + window
    quiet_since = None
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if not line:
            if not seen and time.time() > deadline - window + NO_NODE_GRACE:
                break            # nobody of this env is listening; move on
            if seen and all(v in ("ok", "failed") for v in seen.values()):
                if quiet_since is None:
                    quiet_since = time.time()
                elif time.time() - quiet_since > 3:
                    break
            continue
        if PREFIX not in line:
            continue
        body = line[line.index(PREFIX):]
        if " id=" not in body:
            log("    host: " + body[len(PREFIX) + 1:])
            continue
        f = fields_of(body)
        nid, state = f.get("id"), f.get("state")
        if not nid or not state:
            continue
        if seen.get(nid) != state:
            seen[nid] = state
            quiet_since = None
            err = f.get("err", "0")
            detail = f"  (err {err})" if state == "failed" and err != "0" else ""
            log(f"    {f.get('name', nid)}  {f.get('ip','')}  {state}{detail}")
    return seen


def serve_round(host_id, port_hint, chip, env, image, md5, id_filter, window):
    """Write the image to the host and let the nodes pull it. One attempt."""
    port, _ = resolve_port(host_id, port_hint)
    if not port:
        sys.exit(f"Lost the host board ({host_id}) — is it still plugged in?")

    s = open_port(port)
    slot = ask(s, "OTA-SLOT", " slot ")
    s.close()
    if not slot or "offset" not in slot:
        sys.exit("Host could not name a spare OTA slot. Is it on a partition "
                 "table with two app slots?")
    if len(image) > int(slot["size"], 16):
        sys.exit(f"{env} image ({len(image)} bytes) does not fit the host's "
                 f"{int(slot['size'], 16)}-byte slot.")

    # esptool resets the board, so every round starts from a fresh host boot.
    if not write_image(port, chip, slot["offset"], firmware_for(env)):
        sys.exit("esptool FAILED writing the image to the host.")
    time.sleep(2.0)

    port, _ = resolve_port(host_id, port)
    if not port:
        sys.exit(f"Host board ({host_id}) did not come back after the write.")

    s = open_port(port)
    try:
        started = ask(s, f"OTA-SERVE env={env} size={len(image)} md5={md5} "
                         f"id={id_filter:04X}", "serving env=", timeout=30)
        if not started:
            log("    host refused to serve this image (see its md5 output above)")
            return {}, port
        return watch(s, window), port
    finally:
        s.close()


def push_env(host_id, port, chip, env, id_filter, window, tries, do_build):
    """Push one env, retrying the whole round if any board reports a failure."""
    log(f"\n── {env} ──")
    if do_build:
        build(env)
    image = open(firmware_for(env), "rb").read()
    md5 = hashlib.md5(image).hexdigest()
    log(f"  image {len(image)} bytes  md5={md5}")

    seen = {}
    for attempt in range(1, tries + 1):
        seen, port = serve_round(host_id, port, chip, env, image, md5, id_filter, window)
        failed = [k for k, v in seen.items() if v != "ok"]
        if not failed:
            break
        if attempt < tries:
            # A host that answers 404 does so for its whole boot, and the next
            # round starts by resetting it -- so retrying is also the fix.
            log(f"  {len(failed)} board(s) did not take it; retrying "
                f"({attempt + 1}/{tries}) with a fresh host…")
    return seen, port


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("env", nargs="?", default=None)
    ap.add_argument("-i", dest="ident", default=None)
    ap.add_argument("-p", dest="port", default=None)
    ap.add_argument("--window", type=int, default=120)
    ap.add_argument("--tries", type=int, default=3)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--keep-host", action="store_true")
    ap.add_argument("--no-survey", action="store_true")
    a = ap.parse_args()

    id_filter = 0
    if a.ident:
        if not a.env:
            sys.exit("-i needs an env too: a board's env cannot be known before "
                     "it is invited. e.g. ota.py sensor -i A855")
        t = a.ident.upper()
        if t.startswith("N-"):
            t = t[2:]
        try:
            id_filter = int(t, 16)
        except ValueError:
            sys.exit(f"-i wants a device ID like A855 or N-A855, not {a.ident!r}")

    # Find the tethered board and learn what it is. Everything after this
    # addresses it by device ID, because resets move its /dev path around.
    port, ident = resolve_port(None, a.port, timeout=20.0)
    if not ident:
        sys.exit("No board answered on USB. Plug one in — it is the host.")
    host_id, host_env = ident.get("id", "?"), ident.get("env", "?")
    chip = CHIP_FOR_CLASS.get(ident.get("class", "?"))
    if not chip:
        sys.exit(f"Host board reports class={ident.get('class')}, which this tool "
                 "does not know.")
    log(f"Host board: {ident.get('name','?')} on {port} "
        f"(class={ident.get('class')}, running {host_env})")

    if not a.no_survey:
        log("\nBefore — what is out there:")
        survey.render(survey.collect(port, survey.DEFAULT_SPREAD_MS))

    envs = [a.env] if a.env else live_envs()
    log("\nPushing: " + ", ".join(envs) + (f"  (only N-{a.ident})" if a.ident else ""))

    results = {}
    for env in envs:
        seen, port = push_env(host_id, port, chip, env, id_filter,
                              a.window, a.tries, not a.no_build)
        results[env] = seen

    # The host cannot pull from its own AP, so it gets the wired path. Only in
    # the full run: a targeted push should touch exactly what was targeted.
    host_flashed = False
    if not a.env and not a.keep_host:
        log(f"\n── {host_env} (the tethered board, over its cable) ──")
        port, _ = resolve_port(host_id, port)
        r = subprocess.run([FLASH_ALL, host_env, "-p", port], cwd=ROOT,
                           capture_output=True, text=True)
        sys.stdout.write("".join("  " + l + "\n" for l in r.stdout.splitlines()[-4:]))
        host_flashed = r.returncode == 0

    if not a.no_survey:
        port, _ = resolve_port(host_id, port)
        if port:
            log("\nAfter — what is out there now:")
            survey.render(survey.collect(port, survey.DEFAULT_SPREAD_MS))

    log("")
    total_ok, total_bad = [], []
    for env, seen in results.items():
        ok = [k for k, v in seen.items() if v == "ok"]
        bad = [k for k, v in seen.items() if v != "ok"]
        total_ok += ok
        total_bad += bad
        state = ", ".join("N-" + k for k in ok) if ok else "no boards answered"
        log(f"{env:<14} {state}")
        if bad:
            log(f"{'':<14} FAILED: " + " ".join(f"N-{k}({seen[k]})" for k in bad))
    if host_flashed:
        log(f"{host_env:<14} N-{host_id} (over the cable)")

    log("")
    log(f"Flashed {len(total_ok) + (1 if host_flashed else 0)} board(s)"
        + (f", {len(total_bad)} failed" if total_bad else "."))
    if total_bad:
        log("Run it again — a failed board is simply invited once more.")
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
