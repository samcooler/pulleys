"""Waveforms: decoded once, cached on disk, redone when the file changes."""
from pathlib import Path
import shutil
import sys
import tempfile
import time
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'overlord'))
import waveform


def write_wav(path, seconds, amp):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(int(amp * 32767).to_bytes(2, 'little', signed=True) * int(8000 * seconds))


@unittest.skipUnless(shutil.which('ffmpeg') or shutil.which('mpv'), 'needs ffmpeg or mpv')
class WaveformTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.sound = self.root / 'a.wav'
        write_wav(self.sound, 2, 0.5)
        self.cache = waveform.WaveformCache(lambda cue: [self.sound] if cue == 'a' else [], self.root / 'c.json')

    def tearDown(self):
        self.dir.cleanup()

    def wait(self, cue='a'):
        for _ in range(100):
            out, pending = self.cache.waves([cue])
            if not pending:
                return out
            time.sleep(0.1)
        self.fail('waveform never finished')

    def test_duration_and_peaks(self):
        out = self.wait()['a']
        self.assertAlmostEqual(out['d'], 2, places=1)
        self.assertEqual(len(out['p']), waveform.BINS)
        self.assertAlmostEqual(max(out['p']), 0.5, places=1)

    def test_cached_on_disk_and_redone_when_changed(self):
        self.wait()
        again = waveform.WaveformCache(lambda cue: [self.sound], self.root / 'c.json')
        out, pending = again.waves(['a'])    # from the file: ready at once, nothing to do
        self.assertEqual(pending, 0)
        self.assertIn('a', out)
        write_wav(self.sound, 1, 0.25)
        out, pending = again.waves(['a'])
        self.assertNotIn('a', out)           # stale: being redrawn
        self.assertAlmostEqual(self.wait_on(again)['a']['d'], 1, places=1)

    def wait_on(self, cache):
        for _ in range(100):
            out, pending = cache.waves(['a'])
            if not pending:
                return out
            time.sleep(0.1)
        self.fail('waveform never finished')

    def test_cue_with_no_file_is_skipped(self):
        self.assertEqual(self.cache.waves(['nope']), ({}, 0))


if __name__ == '__main__':
    unittest.main()
