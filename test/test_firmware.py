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


if __name__ == "__main__":
    unittest.main()
