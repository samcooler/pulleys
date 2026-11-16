"""Restart, update or reboot this Pi on a word from the mesh.

For a Pi that is off the network but still on the mesh. Any bridge can send
MESH_MSG_PI_CONTROL (tools/pi_control.py, through a bridge on a laptop's USB);
every bridge hands it to its Pi as a line

    PI <action> <target>

action  restart   the app quits; systemd starts it again (Restart=always),
                  reading its code and serial port afresh
        update    git pull --ff-only in this checkout, then restart
        reboot    the whole Pi (sudo -n systemctl reboot: install.sh allows
                  just that without a password)
target  a Pi's player name (overlord, diorama, ...), or all

A line for another Pi is ignored. The mesh dedupes the copies of one message,
but a second message within SETTLE_S is ignored too, so a sender that repeats
itself does not restart the app twice.
"""

import subprocess
import threading
import time
from pathlib import Path

ACTIONS = ("restart", "update", "reboot")
SETTLE_S = 30.0
REPO = Path(__file__).resolve().parent.parent

_last = [None]   # when the last one was acted on; None = never


def parse(line):
    """(action, target) from a "PI ..." line, or None if it is not one."""
    parts = line.split()
    if len(parts) != 3 or parts[0] != "PI" or parts[1] not in ACTIONS:
        return None
    return parts[1], parts[2]


def handle(line, me, quit_app, clock=time.monotonic, run=subprocess.run):
    """True if `line` was a PI control line (acted on or not). `me` is this
    Pi's player name; quit_app() ends the app so systemd restarts it."""
    parsed = parse(line)
    if not parsed:
        return False
    action, target = parsed
    if target not in (me, "all"):
        print(f"pi-control: {action} for {target}, not us ({me})", flush=True)
        return True
    now = clock()
    if _last[0] is not None and now - _last[0] < SETTLE_S:
        print(f"pi-control: {action} ignored, one was just done", flush=True)
        return True
    _last[0] = now
    print(f"pi-control: {action} (from the mesh)", flush=True)
    # Off the serial reader's thread: a pull or a reboot takes a while.
    threading.Thread(target=_act, args=(action, quit_app, run), daemon=True).start()
    return True


def _act(action, quit_app, run):
    try:
        if action == "reboot":
            r = run(["sudo", "-n", "systemctl", "reboot"], capture_output=True, text=True, timeout=30)
            if r.returncode != 0:
                print(f"pi-control: reboot refused ({(r.stderr or '').strip()}); restarting the app instead"
                      " -- rerun overlord/deploy/install.sh to allow it", flush=True)
                quit_app()
            return
        if action == "update":
            r = run(["git", "-C", str(REPO), "pull", "--ff-only"], capture_output=True, text=True, timeout=120)
            print(f"pi-control: git pull: {(r.stdout or r.stderr).strip().splitlines()[-1:]}", flush=True)
        quit_app()
    except Exception as e:
        print(f"pi-control: {action} failed: {e}; restarting the app", flush=True)
        quit_app()
