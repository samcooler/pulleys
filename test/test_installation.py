"""Installation protocol assembly, retry semantics, API and transport failures."""
import http.client
import json
from pathlib import Path
import sys
import subprocess
import threading
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'overlord'))
from installation import InstallationError, InstallationManager
from control import ControlServer


BOOT = 'B' * 16


def installation(node='ABCD', revision=1):
    return {'id': node, 'boot': BOOT, 'revision': revision, 'storage': 'saved',
            'game_shuffled': False,
            'channels': [{'channel': ch, 'color': ch % 6, 'daily_color': ch % 6, 'ropes': 1} for ch in range(1, 25)],
            'colors': [{'name': name, 'rgb': [255, 0, 0]} for name in ['red', 'blue', 'green', 'yellow', 'orange', 'purple']],
            'sensors': []}


def frames(q, response, node='ABCD'):
    text = json.dumps(response, separators=(',', ':'))
    chunks = [text[i:i+200] for i in range(0, len(text), 200)]
    return [f'IC {node} {q} 1234 {i:02} {len(chunks):02} {chunk.encode().hex().upper()}' for i, chunk in enumerate(chunks)]


class Serial:
    connected = True

    def __init__(self):
        self.lines = []
        self.on_write = lambda request: None

    def write_line(self, line):
        self.lines.append(line)
        self.on_write(json.loads(line.removeprefix('INSTALL ')))
        return True


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.serial = Serial()
        self.manager = InstallationManager(self.serial, timeout=.08, retry=.02)
        self.manager._arbiters['ABCD'] = (installation(), time.monotonic())
        self.body = {'target': 'ABCD', 'boot': BOOT, 'revision': 1}

    def reply(self, request, row=None, **extra):
        response = {'v': 1, 'ok': True, 'installation': row or installation(), **extra}
        for frame in reversed(frames(request['q'], response)):
            self.manager.handle_line(frame)

    def test_out_of_order_duplicate_and_wrong_request_chunks(self):
        def send(request):
            response = {'v': 1, 'ok': True, 'installation': installation(revision=2)}
            stale = frames('0'*16, response)
            valid = frames(request['q'], response)
            for frame in stale + list(reversed(valid)) + valid:
                self.manager.handle_line(frame)
        self.serial.on_write = send
        result = self.manager.mutate('shuffle', self.body)
        self.assertTrue(result['ok'])
        self.assertEqual(self.manager.snapshot()['arbiters'][0]['revision'], 2)
        self.assertEqual(len(self.serial.lines), 1)

    def test_retry_reuses_request_id_and_assembles_missing_chunk(self):
        seen = []
        def send(request):
            seen.append(request)
            response = {'v': 1, 'ok': True, 'installation': installation(revision=2)}
            lines = frames(request['q'], response)
            for frame in lines[1:] if len(seen) == 1 else lines:
                self.manager.handle_line(frame)
        self.serial.on_write = send
        self.manager.mutate('shuffle', self.body)
        self.assertEqual(len(seen), 2)
        self.assertEqual(seen[0], seen[1])

    def test_timeout_never_reports_success(self):
        with self.assertRaises(InstallationError) as error:
            self.manager.mutate('shuffle', self.body)
        self.assertEqual(error.exception.status, 504)
        self.assertIn('unknown', str(error.exception))
        self.assertIsNone(self.manager._pending)

    def test_conflict_updates_readback_without_retrying_mutation(self):
        self.serial.on_write = lambda request: self.reply(request, installation(revision=3), ok=False, error='conflict')
        with self.assertRaises(InstallationError) as error:
            self.manager.mutate('shuffle', self.body)
        self.assertEqual(error.exception.status, 409)
        self.assertEqual(self.manager.snapshot()['arbiters'][0]['revision'], 3)
        self.assertEqual(len(self.serial.lines), 1)

    def test_disconnected_busy_and_stale_make_no_writes(self):
        self.serial.connected = False
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.assertFalse(self.manager.snapshot()['arbiters'][0]['fresh'])
        self.serial.connected = True
        self.manager.busy = lambda: True
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.manager.busy = lambda: False
        self.manager._arbiters['ABCD'] = (installation(), time.monotonic() - 20)
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.assertEqual(self.serial.lines, [])

    def test_validate_rejects_coercions_extra_fields_and_out_of_range(self):
        for op, fields in [('color', {'channel': 0, 'color': 2}), ('color', {'channel': 25, 'color': 2}),
                           ('color', {'channel': 1, 'color': True}), ('rgb', {'color': 0, 'r': 256, 'g': 0, 'b': 0}),
                           ('rgb', {'color': 0, 'r': 1.5, 'g': 0, 'b': 0}), ('shuffle', {'q': 'BAD'})]:
            with self.subTest(op=op, fields=fields), self.assertRaises(ValueError):
                self.manager.mutate(op, {**self.body, **fields})
        self.assertFalse(self.serial.lines)

    def test_invalid_chunks_cannot_complete_or_refresh_state(self):
        old = self.manager._arbiters['ABCD'][1]
        def send(request):
            q = request['q']
            malformed = [f'IC ABCD {q} 1234 0 65 7B7D', f'IC ABCD {q} 1234 0 1 ZZ',
                         f'IC ABCD {q} 1234 0 1 ' + '61'*201]
            wrong = frames(q, {'v': 1, 'ok': True, 'installation': installation('EEEE')})
            for frame in malformed + wrong: self.manager.handle_line(frame)
        self.serial.on_write = send
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.assertEqual(self.manager._arbiters['ABCD'][1], old)

    def test_whitespace_and_unknown_messages(self):
        row = installation()
        row['note'] = 'a string with spaces at chunk boundaries ' * 12
        self.serial.on_write = lambda request: self.reply(request, row)
        result = self.manager.mutate('shuffle', self.body)
        self.assertEqual(result['installation']['note'], row['note'])
        self.assertFalse(self.manager.handle_line('cue test'))

    def test_multiple_arbiters_cannot_satisfy_targeted_request(self):
        def send(request):
            for frame in frames(request['q'], {'v': 1, 'ok': True, 'installation': installation('EEEE')}, 'EEEE'):
                self.manager.handle_line(frame)
        self.serial.on_write = send
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.assertNotIn('EEEE', self.manager._arbiters)

    def test_discovery_waits_for_larger_arbiter_response(self):
        self.manager.timeout = 2
        self.manager.retry = 3
        timers = []
        def send(request):
            self.reply(request)
            response = {'v': 1, 'ok': True, 'installation': installation('EEEE')}
            lines = frames(request['q'], response, 'EEEE')
            self.manager.handle_line(lines[0])
            timer = threading.Timer(1.1, lambda: [self.manager.handle_line(line) for line in lines[1:]])
            timers.append(timer)
            timer.start()
        self.serial.on_write = send
        try:
            self.manager.request('get')
            self.assertEqual([row['id'] for row in self.manager.snapshot()['arbiters']], ['ABCD', 'EEEE'])
        finally:
            for timer in timers: timer.join()

    def test_reboot_conflict_replaces_boot_readback(self):
        row = installation()
        row['boot'] = 'C'*16
        self.serial.on_write = lambda request: self.reply(request, row, ok=False, error='conflict')
        with self.assertRaises(InstallationError): self.manager.mutate('shuffle', self.body)
        self.assertEqual(self.manager.snapshot()['arbiters'][0]['boot'], row['boot'])

    def test_mapping_and_clock_payloads(self):
        seen = []
        def send(request):
            seen.append(request)
            self.reply(request)
        self.serial.on_write = send
        session = 'A' * 16
        for op in ('map_begin', 'map_renew', 'map_again', 'map_end'):
            self.manager.mutate(op, {**self.body, 'session': session})
            self.assertEqual(seen[-1]['session'], session)
        self.manager.mutate('map_assign', {**self.body, 'session': session, 'channel': 24})
        self.manager.mutate('clock', {**self.body, 'time': '20240229235959'})
        self.manager.mutate('clock_now', self.body)
        self.assertEqual(seen[-1]['op'], 'clock')
        self.assertEqual(len(seen[-1]['time']), 14)
        for text in ('20250229000000', '20260431000000', '20261107240000', '20230101000000', '21000101000000'):
            with self.subTest(time=text), self.assertRaises(ValueError):
                self.manager.mutate('clock', {**self.body, 'time': text})
        with self.assertRaises(ValueError):
            self.manager.mutate('map_begin', {**self.body, 'session': '0'*16})

    def test_native_mapping_controller_and_calendar(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'installation-core')
            subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                            str(root / 'test/installation_core.cpp'), '-o', binary], check=True, capture_output=True)
            subprocess.run([binary], check=True, capture_output=True)

    def test_real_http_routes_validation_and_static_page(self):
        self.serial.on_write = lambda request: self.reply(request)
        server = ControlServer(lambda *_: (200, {'legacy': True}), port=0, installation=self.manager)
        server.start()
        try:
            connection = http.client.HTTPConnection(*server.address, timeout=2)
            connection.request('GET', '/installation')
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b'Channel colours', response.read())
            connection.request('GET', '/api/installation')
            response = connection.getresponse()
            self.assertTrue(json.loads(response.read())['arbiters'][0]['fresh'])
            connection.request('POST', '/api/installation/color', json.dumps({**self.body, 'channel': 1, 'color': 2}), {'Content-Type': 'application/json'})
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertTrue(json.loads(response.read())['ok'])
            connection.request('POST', '/api/installation/rgb', '{}')
            response = connection.getresponse()
            self.assertEqual(response.status, 400)
            response.read()
            self.manager.busy = lambda: True
            connection.request('POST', '/api/installation/shuffle', json.dumps(self.body))
            response = connection.getresponse()
            self.assertEqual(response.status, 409)
            response.read()
            connection.request('GET', '/status')
            response = connection.getresponse()
            self.assertEqual(json.loads(response.read()), {'legacy': True})
            connection.close()
        finally:
            server.stop()


if __name__ == '__main__':
    unittest.main()
