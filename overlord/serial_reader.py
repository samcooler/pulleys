"""Read line-based events from serial port or Unix socket."""

import glob
import os
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:
    serial = None

BAUD = 115200
RECONNECT_DELAY = 2.0
SOCKET_PATH = os.environ.get("OVERLORD_SOCK", "/tmp/overlord.sock")


def find_serial_port():
    """Auto-detect ESP32 serial port, returns None if not found."""
    # macOS patterns
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
    # Linux patterns
    ports += glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    return sorted(ports)[0] if ports else None


class SerialReader:
    def __init__(self, on_event, test_mode=False, port=None):
        """
        on_event: callback(event_name: str) called for each event
        test_mode: if True, listen on Unix socket instead of serial
        port: explicit port path (None = auto-detect)
        """
        self.on_event = on_event
        self.test_mode = test_mode
        self.port = port
        self._running = False
        self._thread = None

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
            try:
                s = serial.Serial()
                s.port = port
                s.baudrate = BAUD
                s.timeout = 0.3
                # Prevents CH340 boards from resetting
                s.dtr = False
                s.rts = False
                s.open()
                s.dtr = False
                s.rts = False
                time.sleep(0.1)
                s.reset_input_buffer()
                print(f"serial: connected to {port}", flush=True)

                # Read loop
                while self._running:
                    try:
                        line = s.readline()
                        if not line:
                            continue
                        text = line.decode("utf-8", "replace").strip()
                        if text:
                            self.on_event(text)
                    except serial.SerialException:
                        break
                    except Exception as e:
                        print(f"serial read error: {e}", flush=True)

            except Exception as e:
                print(f"serial error on {port}: {e}", flush=True)
            finally:
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
