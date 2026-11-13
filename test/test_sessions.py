"""The activity log: sessions from state and pull records, replayed from disk."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'overlord'))
from sessionlog import SessionLog

PHASES = ['passive', 'easy_round', 'medium_round', 'hard_round', 'kickout']


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


def state(name):
    return {'kind': 'state', 'name': name, 'prev': None}


def pull(cls='goal'):
    return {'kind': 'pull', 'ch': 3, 'color': 'red', 'cls': cls, 'mag': 5}


class SessionLogTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / 'activity.jsonl'
        self.clock = Clock()
        self.log = SessionLog(self.path, 'passive', PHASES, clock=self.clock)

    def tearDown(self):
        self.log.close()
        self.dir.cleanup()

    def at(self, dt, *records):
        self.clock.t += dt
        for rec in records:
            self.log.feed(rec)

    def test_session_figures(self):
        self.at(0, state('passive'))
        self.at(10, state('easy_round'), pull(), pull('nongoal'))
        self.at(30, state('medium_round'), pull())
        self.at(20, state('easy_round'))          # a lower phase later does not lower the max
        self.at(5, state('passive'))
        self.at(100, state('easy_round'), pull())
        snap = self.log.snapshot()
        live, first = snap['sessions']
        self.assertEqual((first['n'], first['pulls'], first['goal_pulls']), (1, 3, 2))
        self.assertEqual(first['max_phase'], 'medium_round')
        self.assertEqual(first['duration'], 55)
        self.assertIsNone(first['gap'])
        self.assertEqual(first['phases'], [['easy_round', 30], ['medium_round', 20], ['easy_round', 5]])
        self.assertTrue(live['live'])
        self.assertEqual(live['gap'], 100)
        self.assertEqual(live['pulls'], 1)

    def test_pulls_while_idle_are_not_a_session(self):
        self.at(0, state('passive'), pull())
        self.assertEqual(self.log.snapshot()['total'], 0)

    def test_replay_from_disk(self):
        self.at(0, state('passive'), state('hard_round'), pull())
        self.at(9, state('kickout'))
        self.at(1, state('passive'))
        again = SessionLog(self.path, 'passive', PHASES, clock=self.clock)
        rows = again.snapshot()['sessions']
        self.assertEqual([(s['n'], s['pulls'], s['max_phase'], s['live']) for s in rows],
                         [(1, 1, 'kickout', False)])
        self.at(5, state('easy_round'))
        self.assertEqual(self.log.snapshot()['sessions'][0]['n'], 2)
        again.close()

    def test_restart_mid_session_continues_it(self):
        self.at(0, state('passive'), state('easy_round'), pull())
        again = SessionLog(self.path, 'passive', PHASES, clock=self.clock)
        again.feed(state('easy_round'))           # the heartbeat after the app came back
        self.assertEqual(again.snapshot()['total'], 1)
        self.assertEqual(len(again.snapshot()['sessions'][0]['phases']), 1)
        again.close()

    def test_game_reload_ends_the_session(self):
        self.at(0, state('easy_round'))
        self.at(4, {'kind': 'start', 'game': 'g'})
        row = self.log.snapshot()['sessions'][0]
        self.assertFalse(row['live'])
        self.assertEqual(row['why'], 'restart')

    def test_log_lines_are_terse_json(self):
        self.at(0, state('easy_round'), pull())
        self.log.close()
        events = [json.loads(line) for line in self.path.read_text().splitlines()]
        self.assertEqual([e['e'] for e in events], ['start', 'pull'])
        self.assertEqual(events[1]['c'], 'red')


if __name__ == '__main__':
    unittest.main()
