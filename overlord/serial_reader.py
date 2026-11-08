"""Read line-based events from serial port or Unix socket."""

import glob
import os
import re
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:
    serial = None

BAUD = 921600
LEGACY_BAUD = 115200
RECONNECT_DELAY = 2.0
SOCKET_PATH = os.environ.get("OVERLORD_SOCK", "/tmp/overlord.sock")


def find_serial_port():
    """Auto-detect the bridge's serial port, returns None if not found.

    In order of preference: the bridge's own USB port (a C3 enumerates as
    ttyACM / usbmodem), then a USB-serial adapter, then the Pi's GPIO UART. Not
    a plain sort: /dev/serial0 exists on every Pi and sorts ahead of ttyACM0,
    which would pick the empty GPIO UART over a bridge plugged in by USB."""
    for pattern in ("/dev/ttyACM*", "/dev/cu.usbmodem*",
                    "/dev/ttyUSB*", "/dev/cu.usbserial*",
                    "/dev/serial0", "/dev/ttyAMA*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


class SerialReader:
    def __init__(self, on_event, test_mode=False, port=None, baud=BAUD):
        """
        on_event: callback(event_name: str) called for each event
        test_mode: if True, listen on Unix socket instead of serial
        port: explicit port path (None = auto-detect)
        """
        self.on_event = on_event
        self.test_mode = test_mode
        self.port = port
        self.baud = int(baud)
        if self.baud <= 0:
            raise ValueError("serial baud must be positive")
        self._bridge_id = None
        self._running = False
        self._thread = None
        self._serial = None
        self._write_lock = threading.Lock()

    @property
    def connected(self):
        return self._serial is not None and self._serial.is_open

    @property
    def baudrate(self):
        return self._serial.baudrate if self.connected else None

    def _probe_baud(self, port):
        """Identify the bridge before allowing workers to write to this port.

        Old bridges still boot at 115200. A bridge self-update can change its
        boot rate, so every connection probes preferred, fast, and legacy rates.
        Native USB accepts either host rate without a physical UART change.
        """
        self._bridge_id = None
        for rate in dict.fromkeys((self.baud, BAUD, LEGACY_BAUD)):
            if not self._running:
                return None
            try:
                port.baudrate = rate
                port.reset_input_buffer()
            except (OSError, ValueError):
                continue
            deadline = time.monotonic() + 2
            next_ping = 0
            while self._running and time.monotonic() < deadline:
                now = time.monotonic()
                if now >= next_ping:
                    port.write(b"FW-PING\n")
                    port.flush()
                    next_ping = now + .4
                raw = port.readline()
                text = raw.decode("ascii", "ignore").strip()
                if not text.startswith("FW-BRIDGE "):
                    continue
                fields = dict(token.split("=", 1) for token in text.split()[1:] if "=" in token)
                if re.fullmatch(r"[0-9A-Fa-f]{4}", fields.get("id", "")) and fields.get("env", "").startswith("bridge"):
                    self._bridge_id = fields["id"].upper()
                    return text
        return None

    def _bridge_rebooting(self, text):
        if not text.startswith("FW-STATUS "):
            return False
        fields = dict(token.split("=", 1) for token in text.split()[1:] if "=" in token)
        return fields.get("id", "").upper() == self._bridge_id and fields.get("state") == "ok"

    def write_line(self, line):
        """Write one control line to the connected bridge UART, if available."""
        with self._write_lock:
            port = self._serial
            if port is None or not getattr(port, "is_open", False):
                return False
            try:
                port.write((line.rstrip("\r\n") + "\n").encode("utf-8"))
                port.flush()
                return True
            except Exception:
                return False

    def start(self):
        """Start the reader thread."""
        if self._thread is not None:
            return
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def stop(self):
        """Clean shutdown."""
        if self._thread is None:
            return
        self._running = False
        self._thread.join(timeout=3.0)
        self._thread = None

    def _read_loop(self):
        """Main thread function - reads events and dispatches them."""
        if self.test_mode:
            self._read_socket_loop()
        else:
            self._read_serial_loop()

    def _read_serial_loop(self):
        """Serial port reading with auto-reconnect."""
        if serial is None:
            print("pyserial not installed; run: pip install pyserial", flush=True)
            print(f"falling back to test mode on {SOCKET_PATH}", flush=True)
            self._read_socket_loop()
            return

        while self._running:
            # Find port
            port = self.port or find_serial_port()
            if not port:
                print(f"no serial port found; using test mode on {SOCKET_PATH}", flush=True)
                self._read_socket_loop()
                return

            # Connect to port
            s = None
            try:
                s = serial.Serial()
                s.port = port
                s.baudrate = self.baud
                s.timeout = 0.3
                # Prevents CH340 boards from resetting
                s.dtr = False
                s.rts = False
                s.open()
                s.dtr = False
                s.rts = False
                time.sleep(0.1)
                identity = self._probe_baud(s)
                if identity is None:
                    raise RuntimeError("no bridge reply at preferred/legacy serial rates")
                with self._write_lock:
                    self._serial = s
                print(f"serial: connected to {port} at {s.baudrate} baud", flush=True)
                self.on_event(identity)

                # Read loop
                while self._running:
                    try:
                        line = s.readline()
                        if not line:
                            continue
                        text = line.decode("utf-8", "replace").strip()
                        if text:
                            self.on_event(text)
                            if self._bridge_rebooting(text):
                                # Re-probe after its install ACK: the new image
                                # may boot at a different baud rate. Do not
                                # wait for USB-UART to disconnect physically.
                                print("serial: bridge rebooting; rechecking baud", flush=True)
                                break
                    except serial.SerialException:
                        break
                    except Exception as e:
                        print(f"serial read error: {e}", flush=True)

            except Exception as e:
                print(f"serial error on {port}: {e}", flush=True)
            finally:
                with self._write_lock:
                    if self._serial is s:
                        self._serial = None
                try:
                    s.close()
                except Exception:
                    pass

            if self._running:
                print(f"serial: disconnected, reconnecting in {RECONNECT_DELAY}s...", flush=True)
                time.sleep(RECONNECT_DELAY)

    def _read_socket_loop(self):
        """Unix socket reading with auto-recreate on errors."""
        while self._running:
            try:
                # Clean up old socket
                try:
                    os.unlink(SOCKET_PATH)
                except FileNotFoundError:
                    pass

                # Create socket
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.bind(SOCKET_PATH)
                sock.listen(1)
                sock.settimeout(1.0)  # Allow checking _running periodically
                print(f"test mode: listening on {SOCKET_PATH}", flush=True)
                print(f"send events with: ./send.sh event_name", flush=True)

                while self._running:
                    try:
                        conn, _ = sock.accept()
                        try:
                            # Read all lines from this connection
                            f = conn.makefile("r")
                            for line in f:
                                text = line.strip()
                                if text:
                                    self.on_event(text)
                        finally:
                            conn.close()
                    except socket.timeout:
                        continue
                    except Exception as e:
                        print(f"socket connection error: {e}", flush=True)

            except Exception as e:
                print(f"socket error: {e}", flush=True)
                if self._running:
                    time.sleep(RECONNECT_DELAY)
            finally:
                try:
                    sock.close()
                except Exception:
                    pass
                try:
                    os.unlink(SOCKET_PATH)
                except Exception:
                    pass

            if self._running:
                time.sleep(0.1)
