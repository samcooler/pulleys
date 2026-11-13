"""Live gain overrides on top of the game YAML's audio_gain."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'overlord'))
import gain

YAML = """
audio_gain:
  audio_loop_idle: 0.89
  audio_once_steam: 0.6
cues:
  audio_loop: [audio_loop_idle, audio_loop_off]
  audio_once: [audio_once_steam, audio_once_chime]
  video_once: [video_once_janis_nag_*]
  game: [game_reset_components]
"""


class GainStoreTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        path = Path(self.dir.name) / 'g.yaml'
        path.write_text(YAML)
        self.store = gain.GainStore(path)

    def tearDown(self):
        self.dir.cleanup()

    def rows(self):
        return {c['cue']: c for c in self.store.snapshot()['cues']}

    def test_lists_sound_cues_only(self):
        self.assertEqual(list(self.rows()), ['audio_loop_idle', 'audio_once_steam', 'audio_once_chime',
                                             'video_once_janis_nag_*'])

    def test_override_and_reset(self):
        self.assertEqual(self.store.get('audio_once_steam'), 0.6)
        self.store.set('audio_once_steam', 1.5)
        self.assertEqual(self.store.get('audio_once_steam'), 1.5)
        self.assertTrue(self.rows()['audio_once_steam']['overridden'])
        self.store.reset('audio_once_steam')
        self.assertEqual(self.store.get('audio_once_steam'), 0.6)
        self.assertFalse(self.rows()['audio_once_steam']['overridden'])

    def test_setting_the_yaml_value_is_not_an_override(self):
        self.store.set('audio_once_steam', 0.6)
        self.assertEqual(self.store.snapshot()['overrides'], 0)

    def test_reset_all(self):
        self.store.set('audio_once_chime', 2)
        self.store.set('video_once_janis_nag_*', 0.5)
        self.assertEqual(self.store.snapshot()['overrides'], 2)
        self.store.reset()
        self.assertEqual(self.store.snapshot()['overrides'], 0)
        self.assertEqual(self.store.get('audio_once_chime'), 1.0)

    def test_rejects_bad_values(self):
        for bad in (-1, 11, 'loud', None, True, float('nan')):
            with self.assertRaises(ValueError):
                self.store.set('audio_once_chime', bad)

    def test_set_many_is_all_or_nothing(self):
        self.store.set_many({'audio_once_steam': 0.3, 'audio_once_chime': 2})
        self.assertEqual(self.store.get('audio_once_steam'), 0.3)
        with self.assertRaises(ValueError):
            self.store.set_many({'audio_once_steam': 5, 'audio_once_chime': 99})
        self.assertEqual(self.store.get('audio_once_steam'), 0.3)

    def test_missing_cues_sort_last(self):
        self.store.media = lambda cue: [] if cue in ('audio_loop_idle', 'audio_once_chime') else ['x']
        rows = self.store.snapshot()['cues']
        self.assertEqual([c['cue'] for c in rows],
                         ['audio_once_steam', 'video_once_janis_nag_*', 'audio_loop_idle', 'audio_once_chime'])
        self.assertEqual([c['missing'] for c in rows], [False, False, True, True])

    def test_yaml_block(self):
        self.store.set('audio_once_chime', 1.25)
        self.store.set('audio_once_steam', 1)   # explicit in the YAML, so kept
        self.assertEqual(self.store.yaml_block(),
                         "audio_gain:\n  audio_loop_idle: 0.89\n  audio_once_steam: 1\n  audio_once_chime: 1.25\n")

    def test_unlisted_gain_still_shown(self):
        self.store.set('audio_once_gone', 2)
        self.assertEqual(self.rows()['audio_once_gone']['group'], 'unlisted')


if __name__ == '__main__':
    unittest.main()
