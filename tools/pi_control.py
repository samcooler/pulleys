#!/usr/bin/env python3
"""Restart, update or reboot a Pi over the mesh, through a bridge on this machine's USB.

    tools/pi_control.py restart                 the Overlord's app (systemd starts it again)
    tools/pi_control.py update                  git pull on the Overlord, then restart the app
    tools/pi_control.py reboot                  the whole Overlord Pi
    tools/pi_control.py restart --target diorama   a satellite instead; --target all for every Pi
    tools/pi_control.py restart --port /dev/cu.usbserial-0001

For a Pi off the network but still on the mesh: the bridge here floods
MESH_MSG_PI_CONTROL, every bridge hands it to its Pi, and the one named acts
(overlord/pi_control.py). The Pi's bridge must run firmware that knows the
message (bridge_esp32 from 2026-11-15 on), and its app must have the code too.
`update` needs the Pi to have internet; `reboot` needs overlord/deploy/install.sh
to have been rerun since (it allows `systemctl reboot` without a password).

Nothing comes back over the mesh to say it worked: watch the Overlord's screen,
or `tools/overlord_restart.sh --status` once it is reachable.
"""
import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: run with ~/.platformio/penv/bin/python")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=("restart", "update", "reboot"))
    ap.add_argument("--target", default="overlord", help="a Pi's player name, or all (default overlord)")
    ap.add_argument("--port", help="the bridge's serial port (default: the only usbserial one)")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    port = args.port
    if not port:
        ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyUSB*"))
        if len(ports) != 1:
            sys.exit(f"name the bridge's port with --port (found: {', '.join(ports) or 'none'})")
        port = ports[0]

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, args.baud, 0.2
    s.dtr = s.rts = False
    s.open()
    # Opening the port resets this kind of board: let it boot and join the mesh.
    deadline = time.time() + 6
    ready = False
    while time.time() < deadline and not ready:
        ready = "Bridge ready" in s.readline().decode(errors="replace")
    time.sleep(0.5)
    s.write(f"PI-CONTROL {args.action} {args.target}\n".encode())
    deadline = time.time() + 3
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("PI-CONTROL sent"):
            print(f"sent over the mesh from {port}: {args.action} {args.target}")
            return
    sys.exit(f"the bridge on {port} did not confirm; is it a bridge with PI-CONTROL firmware?")


if __name__ == "__main__":
    main()
