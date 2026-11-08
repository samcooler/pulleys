"""Terminal redraws, truthful transfer status, cleanup, and HTTP upload progress."""
import contextlib
import hashlib
from pathlib import Path
import tempfile
import http.server
import io
import os
import threading
import unittest
from unittest.mock import patch

from tools import firmware_cli as cli
from tools.firmware_progress import ProgressDisplay, cells, clean, device_line, percent
from tools.push_firmware import UploadBody, request


class Terminal(io.StringIO):
    def isatty(self):
        return True


def job(state='running', stage='transferring', sent=50, **extra):
    return {'id': 'test', 'state': state, 'device_ids': ['6C37', '1966'],
            'devices': {'6C37': {'state': stage, 'bytes': sent, 'total': 100, **extra}}}


class ProgressTests(unittest.TestCase):
    def terminal(self, width=80, height=24):
        stack = contextlib.ExitStack()
        stack.enter_context(patch.dict(os.environ, {'TERM': 'xterm'}))
        stack.enter_context(patch('tools.firmware_progress.shutil.get_terminal_size', return_value=os.terminal_size((width, height))))
        self.addCleanup(stack.close)
        return Terminal()

    def test_redraws_same_region_and_suppresses_identical_frames(self):
        output = self.terminal()
        items = {'6C37': {'state': 'transferring', 'bytes': 25, 'total': 100}}
        with ProgressDisplay(stream=output) as display:
            display.draw('Job test', items, 'Transfer bytes')
            before = output.getvalue()
            display.draw('Job test', items, 'Transfer bytes')
            self.assertEqual(output.getvalue(), before)
            items['6C37']['bytes'] = 75
            display.draw('Job test', items, 'Transfer bytes')
        text = output.getvalue()
        self.assertIn('\x1b[3F', text)
        self.assertIn(' 25%', text)
        self.assertIn(' 75%', text)
        self.assertTrue(text.startswith('\x1b[?25l'))
        self.assertTrue(text.endswith('\x1b[?25h'))

    def test_percent_is_bytes_not_install_success_and_retry_resets(self):
        self.assertEqual(percent({'state': 'verifying', 'bytes': 100, 'total': 100}), 100)
        self.assertIn('verifying', device_line('6C37', {'state': 'verifying', 'bytes': 100, 'total': 100}, 79))
        self.assertEqual(percent({'state': 'failed', 'bytes': 30, 'total': 100}), 30)
        self.assertEqual(percent({'state': 'offering', 'bytes': 0, 'total': 100, 'attempt': 2}), 0)
        self.assertIn('try 2/3', device_line('6C37', {'state': 'offering', 'attempt': 2}, 79))
        self.assertIsNone(percent({'state': 'skipped'}))
        self.assertIsNone(percent({'state': 'transferring'}))
        self.assertEqual(percent({'state': 'confirmed'}), 100)
        self.assertEqual(percent({'state': 'transferring', 'bytes': 150, 'total': 100}), 100)

    def test_narrow_rows_do_not_wrap_and_percent_stays_on_right(self):
        for width in (1, 5, 12, 24, 39, 79):
            with self.subTest(width=width):
                line = device_line('6C37', {'state': 'transferring', 'bytes': 50, 'total': 100}, width)
                self.assertLessEqual(cells(line), width)
                if width >= 5:
                    self.assertTrue(line.endswith(' 50%'))
        self.assertNotIn('\x1b', clean('failure\x1b[2J\nnext line'))

    def test_large_fleet_keeps_active_device_visible_and_bounds_frame(self):
        output = self.terminal(height=7)
        items = {f'{i:04X}': {'state': 'queued'} for i in range(40)}
        items['FFFF'] = {'state': 'transferring', 'bytes': 70, 'total': 100}
        with ProgressDisplay(stream=output) as display:
            display.draw('Job test', items)
            self.assertLessEqual(display.height, 5)
        self.assertIn('FFFF', output.getvalue())
        self.assertIn('more node(s)', output.getvalue())
        self.assertNotIn('0027', output.getvalue())

    def test_redirected_output_has_no_escapes_and_only_changed_rows(self):
        output = io.StringIO()
        items = {'6C37': {'state': 'transferring', 'bytes': 1, 'total': 100}, '1966': {'state': 'queued'}}
        with ProgressDisplay(stream=output) as display:
            display.draw('Job test', items)
            for value in (2, 5, 9, 10, 11, 19, 20):
                items['6C37']['bytes'] = value
                display.draw('Job test', items)
        text = output.getvalue()
        self.assertNotIn('\x1b', text)
        self.assertEqual(text.count('1966'), 1)
        self.assertEqual(text.count('6C37'), 3)

    def test_waiter_restores_cursor_on_success_interrupt_and_poll_error(self):
        for outcome in ('success', 'interrupt', 'error', 'failure'):
            with self.subTest(outcome=outcome):
                output = self.terminal()
                initial = job()
                if outcome == 'success':
                    reply = {'job': job('completed', 'confirmed', 100)}
                    error = None
                elif outcome == 'failure':
                    reply = {'job': job('failed', 'failed', 75, error='checksum mismatch')}
                    error = RuntimeError
                else:
                    reply = KeyboardInterrupt() if outcome == 'interrupt' else RuntimeError('connection lost')
                    error = type(reply)
                with contextlib.redirect_stdout(output), patch.object(cli.time, 'sleep'), patch.object(cli, 'request', side_effect=[reply]):
                    if error:
                        with self.assertRaises(error): cli.wait_job('http://test', initial)
                    else:
                        self.assertEqual(cli.wait_job('http://test', initial)['state'], 'completed')
                self.assertIn('\x1b[?25h', output.getvalue())
                if outcome == 'error': self.assertIn('job may still be running', output.getvalue())
                if outcome == 'failure': self.assertIn('checksum mismatch', output.getvalue())

    def test_quiet_and_dumb_terminals(self):
        output = self.terminal()
        with contextlib.redirect_stdout(output):
            cli.wait_job('http://test', job('completed', 'confirmed'), quiet=True)
        self.assertEqual(output.getvalue(), '')
        with patch.dict(os.environ, {'TERM': 'dumb'}), ProgressDisplay(stream=output) as display:
            display.draw('Job test', {'6C37': {'state': 'confirmed'}})
        self.assertNotIn('\x1b', output.getvalue())

    def test_upload_rejection_restores_cursor_and_never_queues_devices(self):
        output = self.terminal()
        payload = b'firmware'
        entry = {'asset': 'sensor.bin', 'size': len(payload),
                 'sha256': hashlib.sha256(payload).hexdigest(), 'md5': hashlib.md5(payload).hexdigest()}
        def reject(url, path='', body=None, binary=False, progress=None):
            if path == '/releases': return {'release': {'id': 'release'}}
            if binary:
                progress(len(body), len(body))
                raise RuntimeError('image validation failed')
            self.fail('update must not be queued after upload rejection')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'manifest.json'
            (path.parent / entry['asset']).write_bytes(payload)
            with contextlib.redirect_stdout(output), patch.object(cli, 'request', side_effect=reject), self.assertRaisesRegex(RuntimeError, 'validation failed'):
                cli.upload_and_queue('http://test', path, {'firmware': {'sensor': entry}}, [{'id': 'E857', 'env': 'sensor'}])
        text = output.getvalue()
        self.assertIn('validating', text)
        self.assertIn('failed', text)
        self.assertIn('\x1b[?25h', text)
        self.assertNotIn('uploaded', text)

    def test_resize_reflows_a_bounded_frame(self):
        output = self.terminal()
        items = {'6C37': {'state': 'transferring', 'bytes': 50, 'total': 100}}
        with ProgressDisplay(stream=output) as display:
            display.draw('Job test', items, 'Transfer bytes')
            with patch('tools.firmware_progress.shutil.get_terminal_size', return_value=os.terminal_size((30, 6))):
                display.draw('Job test', items, 'Transfer bytes')
                self.assertLessEqual(display.height, 4)
                self.assertTrue(all(cells(line) <= 29 for line in display.previous))
        self.assertIn('\x1b[2J\x1b[H', output.getvalue())

    def test_upload_callback_advances_after_chunk_is_consumed(self):
        payload = b'x' * (128 * 1024)
        progress = []
        chunks = iter(UploadBody(payload, lambda sent, total: progress.append((sent, total))))
        self.assertEqual(len(next(chunks)), 64 * 1024)
        self.assertEqual(progress, [])
        self.assertEqual(len(next(chunks)), 64 * 1024)
        self.assertEqual(progress, [(64 * 1024, len(payload))])
        with self.assertRaises(StopIteration): next(chunks)
        self.assertEqual(progress[-1], (len(payload), len(payload)))

    def test_http_upload_reports_bytes_and_sends_fixed_length_body(self):
        received = {}
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_PUT(self):
                received['headers'] = self.headers
                received['data'] = self.rfile.read(int(self.headers['Content-Length']))
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(b'{"ok":true}')
            def log_message(self, *args): pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            payload = b'firmware' * 32768
            progress = []
            url = f'http://127.0.0.1:{server.server_port}'
            self.assertTrue(request(url, '/upload', payload, binary=True, progress=lambda sent, total: progress.append((sent, total)))['ok'])
            self.assertEqual(received['data'], payload)
            self.assertIsNone(received['headers'].get('Transfer-Encoding'))
            self.assertGreater(len(progress), 1)
            self.assertEqual(progress[-1], (len(payload), len(payload)))
            self.assertEqual(progress, sorted(progress))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
