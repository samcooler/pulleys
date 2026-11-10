#!/usr/bin/env python3
"""Flash and talk to a board whose CH340 gets no serial port on this Mac.

    tools/ch340_flash.py flash [ENV] [--no-build]   build, then write ENV (default arbiter_crowpanel7)
    tools/ch340_flash.py whoami                     send "?" and print the PULLEYS-ID line
    tools/ch340_flash.py monitor [--reset] [--seconds N]  print serial output until ^C (or N s)
    tools/ch340_flash.py reset                      hard-reset the board

The CrowPanel arbiter's USB chip is a CH340K (1a86:7522). macOS's own driver
only matches 0x7523/0x55D4, and WCH's driver is a system extension that a
managed Mac (MDM) may refuse to enable -- so the board gets no /dev/cu.* port
and neither esptool nor flash_all.sh can see it.

This talks to the chip over libusb instead, in the CH341 vendor protocol (the
same requests Linux's ch341.c sends), and hands it to esptool as a pyserial
port at the URL ch340://. Nothing is installed in the system and no driver is
needed. DTR and RTS work, so esptool's auto-reset into the bootloader does too.

If WCH's driver is enabled after all, it owns the chip and this cannot claim
it: use the /dev/cu.wchusbserial* port with flash_all.sh instead.

Needs libusb (brew install libusb) and pyusb in the Python that runs it:
    ~/.platformio/penv/bin/python -m pip install pyusb
Run it with that Python, which already has esptool's dependencies:
    ~/.platformio/penv/bin/python tools/ch340_flash.py flash
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import types

try:
    import serial
    from serial.serialutil import SerialBase, SerialException, PortNotOpenError
except ImportError:
    sys.exit("missing pyserial: run this with ~/.platformio/penv/bin/python")
try:
    import usb.core
    import usb.util
    import usb.backend.libusb1
except ImportError:
    sys.exit("missing pyusb: ~/.platformio/penv/bin/python -m pip install pyusb")

ROOT = Path(__file__).resolve().parents[1]
PIO_CORE = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
ESPTOOL_DIR = PIO_CORE / "packages/tool-esptoolpy"
BOOT_APP = PIO_CORE / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

VID = 0x1A86
PIDS = (0x7522, 0x7523, 0x5523)   # CH340K first: the one macOS leaves without a port

# ── CH341 vendor protocol (Linux drivers/usb/serial/ch341.c) ──────────────────
REQ_READ_VERSION = 0x5F
REQ_WRITE_REG    = 0x9A
REQ_SERIAL_INIT  = 0xA1
REQ_MODEM_CTRL   = 0xA4
REG_PRESCALER    = 0x12
REG_DIVISOR      = 0x13
REG_LCR          = 0x18
REG_LCR2         = 0x25
LCR_8N1          = 0x80 | 0x40 | 0x03   # RX on, TX on, 8 data bits
BIT_DTR          = 0x20
BIT_RTS          = 0x40
CLKRATE          = 48_000_000
EP_OUT, EP_IN    = 0x02, 0x82


def _clk_div(ps, fact):
    return 1 << (12 - 3 * ps - fact)


def ch341_divisor(speed):
    """ch341_get_divisor(): the prescaler/divisor register value for a baud rate."""
    min_rates = [CLKRATE // (_clk_div(ps, 1) * 512) for ps in range(4)]
    speed = max(-(-CLKRATE // (_clk_div(0, 0) * 256)), min(speed, CLKRATE // (_clk_div(3, 0) * 2)))
    fact = 1
    for ps in range(3, -1, -1):
        if speed > min_rates[ps]:
            break
    else:
        raise ValueError(f"baud {speed} out of range")
    clk_div = _clk_div(ps, fact)
    div = CLKRATE // (clk_div * speed)
    if div < 9 or div > 255:
        div //= 2
        clk_div *= 2
        fact = 0
    if div < 2:
        raise ValueError(f"baud {speed} out of range")
    if 16 * CLKRATE // (clk_div * div) - 16 * speed >= 16 * speed - 16 * CLKRATE // (clk_div * (div + 1)):
        div += 1
    if fact == 1 and div % 2 == 0:
        div //= 2
        fact = 0
    return (0x100 - div) << 8 | fact << 2 | ps


def _backend():
    for lib in (None, "/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"):
        be = usb.backend.libusb1.get_backend(find_library=(lambda _x, lib=lib: lib) if lib else None)
        if be:
            return be
    sys.exit("libusb not found: brew install libusb")


def find_device():
    be = _backend()
    for pid in PIDS:
        dev = usb.core.find(idVendor=VID, idProduct=pid, backend=be)
        if dev is not None:
            return dev
    return None


# ── A pyserial port over libusb, opened as ch340:// ───────────────────────────
class Ch340Serial(SerialBase):
    def open(self):
        self._dev = find_device()
        if self._dev is None:
            raise SerialException("no CH340 (1a86:7522/7523) on USB")
        try:
            self._dev.set_configuration()
        except usb.core.USBError:
            pass   # already configured
        usb.util.claim_interface(self._dev, 0)
        self._buf = bytearray()
        self.is_open = True
        self._ctrl_in(REQ_READ_VERSION, 0, 0, 2)
        self._ctrl_out(REQ_SERIAL_INIT, 0, 0)
        self._reconfigure_port()
        self._send_lines()

    def close(self):
        if getattr(self, "_dev", None) is not None and self.is_open:
            try:
                usb.util.release_interface(self._dev, 0)
                usb.util.dispose_resources(self._dev)
            except usb.core.USBError:
                pass
        self.is_open = False

    def _ctrl_out(self, req, value, index):
        self._dev.ctrl_transfer(0x40, req, value & 0xFFFF, index, None, 1000)

    def _ctrl_in(self, req, value, index, n):
        return self._dev.ctrl_transfer(0xC0, req, value, index, n, 1000)

    def _reconfigure_port(self, force_update=False):
        if not self.is_open:
            return
        # Bit 7: send each byte on, rather than holding it for a full packet.
        self._ctrl_out(REQ_WRITE_REG, REG_DIVISOR << 8 | REG_PRESCALER, ch341_divisor(self._baudrate) | 0x80)
        self._ctrl_out(REQ_WRITE_REG, REG_LCR2 << 8 | REG_LCR, LCR_8N1)

    def _send_lines(self):
        bits = (BIT_DTR if self._dtr_state else 0) | (BIT_RTS if self._rts_state else 0)
        self._ctrl_out(REQ_MODEM_CTRL, ~bits, 0)   # the chip takes the lines inverted

    def _update_dtr_state(self):
        if self.is_open:
            self._send_lines()

    def _update_rts_state(self):
        if self.is_open:
            self._send_lines()

    def set_lines(self, dtr, rts):
        """Both lines in one request: what esptool's tight reset needs."""
        self._dtr_state, self._rts_state = bool(dtr), bool(rts)
        self._send_lines()

    def _pull(self, timeout_ms):
        try:
            data = self._dev.read(EP_IN, 4096, timeout_ms)
            self._buf.extend(data)
            return len(data)
        except usb.core.USBTimeoutError:
            return 0
        except usb.core.USBError as e:
            if getattr(e, "errno", None) == 60 or "timed out" in str(e).lower():
                return 0
            raise

    @property
    def in_waiting(self):
        if not self.is_open:
            raise PortNotOpenError()
        self._pull(1)
        return len(self._buf)

    def read(self, size=1):
        if not self.is_open:
            raise PortNotOpenError()
        deadline = None if self._timeout is None else time.monotonic() + self._timeout
        while len(self._buf) < size:
            left = None if deadline is None else deadline - time.monotonic()
            if left is not None and left <= 0:
                break
            self._pull(max(1, int(min(left if left is not None else 0.05, 0.05) * 1000)))
        out = bytes(self._buf[:size])
        del self._buf[:size]
        return out

    def write(self, data):
        if not self.is_open:
            raise PortNotOpenError()
        data = bytes(data)
        timeout = 5000 if self._write_timeout is None else max(1, int(self._write_timeout * 1000))
        n = 0
        while n < len(data):
            n += self._dev.write(EP_OUT, data[n:n + 4096], timeout)
        return n

    def reset_input_buffer(self):
        self._buf.clear()
        while self._pull(1):
            self._buf.clear()

    def reset_output_buffer(self):
        pass

    def flush(self):
        pass

    def fileno(self):
        raise OSError(25, "no file descriptor: a libusb port")


def register():
    """Make ch340:// a pyserial URL, for serial_for_url and so for esptool."""
    pkg = types.ModuleType("pulleys_ch340")
    pkg.__path__ = []
    mod = types.ModuleType("pulleys_ch340.protocol_ch340")
    mod.Serial = Ch340Serial
    sys.modules["pulleys_ch340"] = pkg
    sys.modules["pulleys_ch340.protocol_ch340"] = mod
    if "pulleys_ch340" not in serial.protocol_handler_packages:
        serial.protocol_handler_packages.append("pulleys_ch340")


def esptool_main(args):
    # PlatformIO's esptool carries its own dependencies in _contrib, and its
    # esptool.py puts that on the path before importing; do the same.
    import site
    site.addsitedir(str(ESPTOOL_DIR / "_contrib"))
    sys.path.insert(0, str(ESPTOOL_DIR / "_contrib"))
    sys.path.insert(0, str(ESPTOOL_DIR))
    import esptool
    from esptool import reset
    # esptool's tight reset sets DTR and RTS together through an ioctl on the
    # port's file descriptor, which a libusb port does not have.
    reset.ResetStrategy._setDTRandRTS = lambda self, dtr=False, rts=False: self.port.set_lines(dtr, rts)
    esptool.main(args)


# ── Commands ──────────────────────────────────────────────────────────────────
def pio():
    for cand in (os.environ.get("PLATFORMIO_CMD"), "pio", str(PIO_CORE / "penv/bin/pio")):
        if cand and (os.path.isfile(cand) or subprocess.run(["which", cand], capture_output=True).returncode == 0):
            return cand
    sys.exit("PlatformIO not found")


def cmd_flash(a):
    build = ROOT / ".pio/build" / a.env
    if not a.no_build:
        print(f"Building {a.env}…")
        if subprocess.run([pio(), "run", "-e", a.env], cwd=ROOT, stdout=subprocess.DEVNULL).returncode:
            sys.exit(f"Build FAILED — not flashing. Run 'pio run -e {a.env}' to see why.")
    for f in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        if not (build / f).is_file():
            sys.exit(f"missing {build / f}: build {a.env} first")
    # The arbiter boards: ESP32-S3, 16 MB -- the same geometry flash_all.sh uses.
    esptool_main(["--chip", "esp32s3", "--port", "ch340://", "--baud", str(a.baud),
                  "--before", "default_reset", "--after", "hard_reset",
                  "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
                  "0x0", str(build / "bootloader.bin"),
                  "0x8000", str(build / "partitions.bin"),
                  "0xe000", str(BOOT_APP),
                  "0x10000", str(build / "firmware.bin")])
    print(f"Done — {a.env} flashed.")


def open_console(reset_first=False):
    # DTR/RTS deasserted, or the auto-reset circuit can hold the chip in reset.
    port = serial.serial_for_url("ch340://", baudrate=115200, timeout=0.1, do_not_open=True)
    port.dtr = False
    port.rts = False
    port.open()
    if reset_first:
        port.set_lines(False, True)    # EN low
        time.sleep(0.1)
        port.set_lines(False, False)
    return port


def cmd_reset(_a):
    open_console(reset_first=True).close()
    print("reset")


def cmd_whoami(a):
    port = open_console()
    port.reset_input_buffer()
    port.write(b"?\n")
    deadline = time.monotonic() + a.wait
    line = b""
    while time.monotonic() < deadline:
        line += port.read(256)
        for raw in line.split(b"\n"):
            if b"PULLEYS-ID" in raw:
                print(raw.decode(errors="replace").strip())
                port.close()
                return
    port.close()
    sys.exit("no PULLEYS-ID reply (is the board running? try: monitor --reset)")


def cmd_monitor(a):
    # A shell starts a background job with SIGINT ignored, and then neither ^C
    # nor kill -INT would ever stop it, leaving the chip claimed. Take both
    # signals back, and TERM too.
    import signal

    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    port = open_console(reset_first=a.reset)
    end = time.monotonic() + a.seconds if a.seconds else None
    try:
        while end is None or time.monotonic() < end:
            data = port.read(512)
            if data:
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        port.close()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("flash", help="build and write an env")
    f.add_argument("env", nargs="?", default="arbiter_crowpanel7")
    f.add_argument("--no-build", action="store_true", help="write what is already in .pio/build")
    f.add_argument("--baud", type=int, default=460800)
    f.set_defaults(fn=cmd_flash)
    w = sub.add_parser("whoami", help='send "?" and print the PULLEYS-ID line')
    w.add_argument("--wait", type=float, default=3.0)
    w.set_defaults(fn=cmd_whoami)
    m = sub.add_parser("monitor", help="print serial output until ^C")
    m.add_argument("--reset", action="store_true", help="reset the board first, to see it boot")
    m.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until ^C)")
    m.set_defaults(fn=cmd_monitor)
    r = sub.add_parser("reset", help="hard-reset the board")
    r.set_defaults(fn=cmd_reset)
    a = p.parse_args()
    if find_device() is None:
        sys.exit("no CH340 (1a86:7522/7523) found on USB")
    register()
    a.fn(a)


if __name__ == "__main__":
    main()
