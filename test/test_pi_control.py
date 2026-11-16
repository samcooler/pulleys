"""overlord/pi_control.py: "PI <action> <target>" lines from the mesh."""
from pathlib import Path
import sys
import threading
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "overlord"))
import pi_control  # noqa: E402


class PiControl(unittest.TestCase):
    def setUp(self):
        pi_control._last[0] = None
        self.quits = []
        self.ran = []
        self.done = threading.Event()
        self.rc = 0
        self.t = 1000.0

    def quit(self):
        self.quits.append(True)
        self.done.set()

    def fake_run(self, cmd, **_):
        self.ran.append(cmd)
        return SimpleNamespace(returncode=self.rc, stdout="Already up to date.\n", stderr="no sudo")

    def send(self, line, me="overlord"):
        return pi_control.handle(line, me, self.quit, clock=lambda: self.t, run=self.fake_run)

    def test_not_a_control_line(self):
        self.assertFalse(self.send("cue audio_once_steam"))
        self.assertFalse(self.send("PI dance overlord"))
        self.assertFalse(self.quits)

    def test_restart_for_us(self):
        self.assertTrue(self.send("PI restart overlord"))
        self.assertTrue(self.done.wait(2))
        self.assertEqual(self.ran, [])

    def test_for_another_pi_is_ignored(self):
        self.assertTrue(self.send("PI restart diorama"))
        self.assertFalse(self.done.wait(0.2))

    def test_all_reaches_a_satellite(self):
        self.assertTrue(self.send("PI restart all", me="diorama"))
        self.assertTrue(self.done.wait(2))

    def test_update_pulls_first(self):
        self.send("PI update overlord")
        self.assertTrue(self.done.wait(2))
        self.assertEqual(self.ran[0][:2], ["git", "-C"])
        self.assertEqual(self.ran[0][-2:], ["pull", "--ff-only"])

    def test_reboot_refused_falls_back_to_restart(self):
        self.rc = 1
        self.send("PI reboot overlord")
        self.assertTrue(self.done.wait(2))
        self.assertEqual(self.ran, [["sudo", "-n", "systemctl", "reboot"]])

    def test_soon_after_boot_still_acts(self):
        self.t = 5.0
        self.send("PI restart overlord")
        self.assertTrue(self.done.wait(2))

    def test_a_repeat_soon_after_is_ignored(self):
        self.send("PI restart overlord")
        self.assertTrue(self.done.wait(2))
        self.done.clear()
        self.t += 5
        self.send("PI restart overlord")
        self.assertFalse(self.done.wait(0.2))
        self.assertEqual(len(self.quits), 1)


if __name__ == "__main__":
    unittest.main()
