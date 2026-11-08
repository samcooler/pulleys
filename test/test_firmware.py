"""Release validation and coordinator regression tests (no hardware required)."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("firmware", ROOT / "overlord/firmware.py")
fw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fw)


class FirmwareTests(unittest.TestCase):
    def test_release_assets(self):
        for optional in (set(), fw.OPTIONAL_ENVS):
            with self.subTest(optional=optional), tempfile.TemporaryDirectory() as directory:
                data = b"firmware-image"
                digest = hashlib.sha256(data).hexdigest()
                envs = fw.ACTIVE_ENVS | optional
                table = {env: {"asset": env + ".bin", "sha256": digest,
                               "build_hash": "a" * 64, "size": len(data),
                               "md5": hashlib.md5(data).hexdigest()} for env in envs}
                assets = [{"name": name, "browser_download_url": name}
                          for name in ["manifest.json", *[env + ".bin" for env in envs]]]
                fetched = []

                def get(url, timeout=15):
                    fetched.append(url)
                    if url.endswith("/releases/latest"):
                        return json.dumps({"tag_name": "v0.2.0", "assets": assets}).encode()
                    if url == "manifest.json":
                        return json.dumps({"tag": "v0.2.0", "schema": 1, "firmware": table}).encode()
                    return data

                with patch.object(fw, "CACHE", Path(directory)), patch.object(fw, "_get", get), \
                     patch.object(fw, "_github_repo", return_value="example/repo"):
                    updater = fw.FirmwareUpdater(None)
                    updater._fetch_latest()
                    for env in envs:
                        self.assertIn(env + ".bin", fetched)
                    self.assertEqual(set(updater._release["manifest"]), envs)
                    table["sensor"]["sha256"] = "0" * 64
                    with self.assertRaisesRegex(RuntimeError, "integrity"):
                        updater._fetch_latest()

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
        with patch.object(updater, "_fetch_latest"), patch.object(fw.time, "sleep"), \
             patch("builtins.print"):
            updater._boot_run()
        self.assertTrue(any(command.startswith("FW-BRIDGE-UPDATE env=bridge_esp32 ")
                            for command in commands))
        self.assertFalse(any(command.startswith("FW-OFFER") for command in commands))
        self.assertEqual(updater._inventory["1966"]["sha256"], expected)


if __name__ == "__main__":
    unittest.main()
