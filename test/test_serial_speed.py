"""Fast/legacy bridge discovery, migration reconnect, and checked host chunks."""
import base64
from collections import deque
import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'overlord'))
import serial_reader as reader
from firmware import FirmwareUpdater


class Port:
    def __init__(self, rate):
        self.good_rate = rate
        self.baudrate = 0
        self.is_open = True
        self.lines = deque()
        self.rates = []
        self.writes = []
    def open(self): self.is_open = True
    def close(self): self.is_open = False
    def reset_input_buffer(self): self.lines.clear(); self.rates.append(self.baudrate)
    def flush(self): pass
    def write(self, data):
        self.writes.append(data)
        if data == b'FW-PING\n' and self.baudrate == self.good_rate:
            self.lines.append(b'FW-BRIDGE id=1966 env=bridge_esp32 sha256=unknown\n')
        return len(data)
    def readline(self): return self.lines.popleft() if self.lines else b''


class SerialSpeedTests(unittest.TestCase):
    def clock(self):
        ticks = [0.0]
        def now(): ticks[0] += .1; return ticks[0]
        return now

    def test_fast_and_legacy_detection(self):
        for rate in (921600, 115200):
            with self.subTest(rate=rate):
                serial = reader.SerialReader(lambda _: None)
                serial._running = True
                port = Port(rate)
                with patch.object(reader.time, 'monotonic', side_effect=self.clock()):
                    self.assertIn('id=1966', serial._probe_baud(port))
                self.assertEqual(port.baudrate, rate)
                self.assertEqual(serial._bridge_id, '1966')
                self.assertFalse(serial.connected)  # no writes until discovery completes
                self.assertTrue(serial._bridge_rebooting('FW-STATUS id=1966 state=ok'))
                self.assertFalse(serial._bridge_rebooting('FW-STATUS id=6C37 state=ok'))
                self.assertFalse(serial._bridge_rebooting('FW-STATUS id=1966 state=failed'))

    def test_reconnect_after_bridge_upgrade_switches_baud(self):
        ports = [Port(115200), Port(921600)]
        reader_obj = None
        events = []
        def event(line):
            events.append(line)
            if line.startswith('FW-BRIDGE'):
                if reader_obj.baudrate == 115200:
                    ports[0].lines.append(b'FW-STATUS id=1966 state=ok\n')
                else:
                    reader_obj._running = False
        reader_obj = reader.SerialReader(event, port='/dev/fake')
        reader_obj._running = True
        module = SimpleNamespace(Serial=iter(ports).__next__, SerialException=OSError)
        with patch.object(reader, 'serial', module), patch.object(reader.time, 'sleep'), \
             patch.object(reader.time, 'monotonic', side_effect=self.clock()), contextlib.redirect_stdout(io.StringIO()):
            reader_obj._read_serial_loop()
        self.assertEqual(sum(line.startswith('FW-BRIDGE') for line in events), 2)
        self.assertEqual(ports[1].baudrate, 921600)
        self.assertTrue(all(not port.is_open for port in ports))

    def test_bounded_detection_and_configured_rate(self):
        serial = reader.SerialReader(lambda _: None, baud=460800)
        serial._running = True
        port = Port(460800)
        with patch.object(reader.time, 'monotonic', side_effect=self.clock()):
            self.assertIsNotNone(serial._probe_baud(port))
        self.assertEqual(port.baudrate, 460800)
        port = Port(0)
        with patch.object(reader.time, 'monotonic', side_effect=self.clock()):
            self.assertIsNone(serial._probe_baud(port))
        self.assertEqual(port.rates, [460800, 921600, 115200])

    def test_usb_identity_probe_finds_fast_bridge(self):
        from tools import identify
        class USBPort(Port):
            def write(self, data):
                if data == b'?\n' and self.baudrate == self.good_rate:
                    self.lines.append(('PULLEYS-ID class=esp32 role=BRIDGE env=bridge_esp32 id=1966 sha256=' + 'a'*64 + '\n').encode())
                return len(data)
            def read(self, size): return self.readline()
        port = USBPort(921600)
        rows = []
        with patch.object(identify.serial, 'Serial', return_value=port), patch.object(identify.time, 'monotonic', side_effect=self.clock()):
            identify.probe('/dev/fake', rows, versions=True)
        self.assertEqual(rows[0]['id'], '1966')
        self.assertEqual(rows[0]['baud'], 921600)

    def test_host_large_legacy_and_invalid_chunk_lengths(self):
        writes = []
        serial = SimpleNamespace(write_line=lambda line: writes.append(line) or True)
        transport = FirmwareUpdater(serial)
        data = b'x' * 4096
        digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'firmware.bin'
            path.write_bytes(data)
            transport._assets[digest] = path
            for length in (384, 4096):
                transport._answer_chunk(f'FW-READ sha256={digest} offset=0 length={length}')
                fields = dict(token.split('=', 1) for token in writes[-1].split()[1:])
                decoded = base64.b64decode(fields['b64'])
                self.assertEqual(decoded, data[:length])
                self.assertEqual(int(fields['crc32'], 16), zlib.crc32(decoded))
                if length == 384: self.assertLess(len(writes[-1]) + 1, 640)
            for length in (0, -1, 4097):
                with contextlib.redirect_stdout(io.StringIO()):
                    transport._answer_chunk(f'FW-READ sha256={digest} offset=0 length={length}')
                self.assertTrue(writes[-1].startswith('FW-ERROR '))
