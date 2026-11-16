#!/usr/bin/env python3
"""A firmware-only Overlord on this machine, driving a bridge plugged in by USB.

For flashing over the mesh when the real Overlord is out of reach: flash any
ESP32 as a bridge (`pio run -e bridge_esp32 -t upload`), then

    ~/.platformio/penv/bin/python tools/laptop_overlord.py --port /dev/cu.usbserial-0001 &
    OVERLORD_URL=http://127.0.0.1:8090 ~/.platformio/penv/bin/python fw flash --env <env>

It runs only the serial link, the firmware manager and the REST API: no media,
no cues, no installation sync. Stop it when done.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "overlord"))
from serial_reader import SerialReader          # noqa: E402
from firmware_manager import FirmwareManager    # noqa: E402
from control import ControlServer               # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the bridge's serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--http-port", type=int, default=8090)
    ap.add_argument("--cache", default=str(Path.home() / ".cache" / "laptop_overlord" / "firmware"))
    args = ap.parse_args()

    manager = None

    def on_line(line):
        line = line.strip()
        if manager and line:
            manager.handle_line(line)

    serial = SerialReader(on_event=on_line, port=args.port, baud=args.baud)
    manager = FirmwareManager(serial, cache=args.cache)
    serial.start()
    manager.start()
    ControlServer(lambda *a, **k: (404, {"error": "firmware only"}), "127.0.0.1", args.http_port,
                  firmware=manager).start()
    print(f"laptop overlord up: OVERLORD_URL=http://127.0.0.1:{args.http_port}", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        manager.stop()
        serial.stop()


if __name__ == "__main__":
    main()
