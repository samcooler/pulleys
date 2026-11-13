"""Release validation and coordinator regression tests (no hardware required)."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("firmware", ROOT / "overlord/firmware.py")
fw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fw)


class FirmwareTests(unittest.TestCase):
    def test_classic_bridge_uses_serial_self_update_and_confirms_reboot(self):
        expected = "a" * 64
        commands = []

        class Serial:
            updated = False

            def write_line(self, line):
                commands.append(line)
                if line == "FW-PING":
                    digest = expected if self.updated else "unknown"
                    updater.handle_line(f"FW-BRIDGE id=1966 env=bridge_esp32 sha256={digest}")
                elif line.startswith("FW-CENSUS"):
                    updater.handle_line("PULLEYS-CENSUS id=1966 env=bridge_esp32 sha256=unknown")
                elif line.startswith("FW-BRIDGE-UPDATE"):
                    self.updated = True
                    updater.handle_line("FW-STATUS id=1966 state=ok")
                return True

        updater = fw.FirmwareUpdater(Serial())
        updater._release = {"tag": "v0.2.0", "manifest": {
            "bridge_esp32": {"build_hash": expected, "sha256": "b" * 64, "size": 1000000}}}
        updater._assets["b" * 64] = Path("unused-test-image.bin")
        with patch.object(fw.time, "sleep"), patch("builtins.print"):
            updater.update_release(updater._release)
        self.assertTrue(any(command.startswith("FW-BRIDGE-UPDATE env=bridge_esp32 ")
                            for command in commands))
        self.assertFalse(any(command.startswith("FW-OFFER") for command in commands))
        self.assertEqual(updater._inventory["1966"]["sha256"], expected)

    def _mesh_updater(self, on_offer, build="a" * 64, sha="b" * 64, size=100000):
        """An updater with one sensor, 'AAAA', that runs on_offer(updater) when offered."""
        class Serial:
            def write_line(self, line):
                if line == "FW-PING":
                    updater.handle_line("FW-BRIDGE id=1966 env=bridge sha256=unknown")
                elif line.startswith("FW-CENSUS"):
                    updater.handle_line("PULLEYS-CENSUS id=AAAA env=sensor sha256=old")
                elif line.startswith(("FW-OFFER", "FW-QUERY")):
                    on_offer(updater, line)
                return True

        updater = fw.FirmwareUpdater(Serial())
        updater._release = {"tag": "t", "manifest": {
            "sensor": {"build_hash": build, "sha256": sha, "md5": "m", "size": size}}}
        updater._assets[sha] = Path("unused-test-image.bin")
        return updater

    def test_silent_device_is_given_up_on_after_the_idle_limit_not_the_transfer_timeout(self):
        updater = self._mesh_updater(lambda u, line: None)
        started = fw.time.monotonic()
        with patch.object(fw, "OFFER_IDLE_SECONDS", 0.2), patch.object(updater, "_settle"), \
                patch("builtins.print"):
            results = updater.update_release(updater._release)
        # Three attempts at ~0.2 s each, where the transfer timeout is 180 s apiece.
        self.assertLess(fw.time.monotonic() - started, 5)
        self.assertEqual(results["AAAA"]["state"], "failed")
        self.assertEqual(results["AAAA"]["error"], "device did not respond to the offer")

    def test_complete_download_with_lost_status_is_verified_by_hash(self):
        build = "a" * 64

        def on_offer(u, line):
            # Served the whole image, then the device went quiet; once it
            # reboots it reports the new build.
            if line.startswith("FW-OFFER"):
                u._note_activity("AAAA", served=100000)
            else:
                u.handle_line(f"FW-NODE id=AAAA env=sensor sha256={build}")

        updater = self._mesh_updater(on_offer, build=build)
        with patch.object(fw, "OFFER_IDLE_SECONDS", 0.2), patch.object(updater, "_settle"), \
                patch("builtins.print"):
            results = updater.update_release(updater._release)
        self.assertEqual(results["AAAA"]["state"], "confirmed")

    def test_chunks_keep_an_offer_alive_past_the_idle_limit(self):
        updater = self._mesh_updater(lambda u, line: None)
        updater._note_activity("AAAA", reset=True)
        updater._note_activity("AAAA", served=5000)
        with patch.object(fw, "OFFER_IDLE_SECONDS", 0.5):
            self.assertIsNone(updater._wait_status("AAAA", 0.3, size=100000, idle=0.5))
        # ...and 0.3 s elapsed with the idle limit untouched, not "silent".
        last, served = updater._activity["AAAA"]
        self.assertEqual(served, 5000)


if __name__ == "__main__":
    unittest.main()
