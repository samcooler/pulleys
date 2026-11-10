"""Development CLI selection, build/flash separation and real HTTP upload tests."""
import contextlib
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from tools import firmware_cli as cli
from tools import release_firmware as bundles


def snapshot():
    return {"devices": [
        {"id": "E857", "env": "sensor", "role": "SENSOR", "sha256": "unknown", "build": "old", "online": True},
        {"id": "EC52", "env": "sensor", "role": "SENSOR", "sha256": "unknown", "build": "old", "online": True},
        {"id": "59A4", "env": "component_state_display", "role": "SCREEN", "sha256": "a" * 64},
        {"id": "1966", "env": "bridge_esp32", "role": "BRIDGE", "sha256": "unknown"},
        {"id": "3EFC", "env": "bridge", "role": "BRIDGE", "sha256": "unknown"}],
        "jobs": [], "releases": [{"tag": "v0.2.0", "manifest": {"component_state_display": {"build_hash": "a" * 64}}}]}


def image_fixture(directory, env="sensor"):
    data = bytearray(128)
    data[0] = 0xe9
    data[12:14] = (9 if env == "sensor" or env.startswith("arbiter") else 5 if env == "bridge" else 0).to_bytes(2, "little")
    data[32:96] = b"a" * 64
    data = bytes(data)
    item = {"asset": env + "-firmware.bin", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "md5": hashlib.md5(data).hexdigest(), "build_hash": "a" * 64}
    manifest = {"schema": 1, "tag": "dev-test", "firmware": {env: item}}
    path = Path(directory) / "manifest.json"
    path.write_text(json.dumps(manifest))
    (path.parent / item["asset"]).write_bytes(data)
    return path, manifest


class CLITests(unittest.TestCase):
    def invoke(self, arguments):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = cli.main(arguments)
        return code, output.getvalue(), errors.getvalue()

    def test_list_defaults_to_overlord_and_reports_versions(self):
        with patch.object(cli, "request", return_value=snapshot()) as request:
            code, output, _ = self.invoke(["list", "--json"])
        self.assertEqual(code, 0)
        rows = json.loads(output)
        self.assertEqual(next(row for row in rows if row["id"] == "59A4")["version"], "v0.2.0")
        self.assertEqual(request.call_args.args[0], "http://overlord.local:8080")

    def test_comma_lists_and_combined_filters_narrow_selection(self):
        with patch.object(cli, "request", return_value=snapshot()):
            code, output, _ = self.invoke(["list", "--id", "N-e857,1966,3EFC", "--type", "sensor,bridge", "--env", "sensor,bridge_esp32", "--json"])
        self.assertEqual(code, 0)
        self.assertEqual({row["id"] for row in json.loads(output)}, {"E857", "1966"})

    def test_broad_bridge_role_includes_both_chip_variants(self):
        with patch.object(cli, "request", return_value=snapshot()):
            _, output, _ = self.invoke(["list", "--type", "bridge", "--json"])
        self.assertEqual({row["env"] for row in json.loads(output)}, {"bridge", "bridge_esp32"})

    def test_missing_id_or_empty_filter_never_builds_or_flashes(self):
        for arguments in (["flash", "--id", "E857,FFFF"], ["flash", "--type", "sensor,,screen"],
                          ["flash", "--type", "typo"], ["flash", "--env", "typo"]):
            with self.subTest(arguments=arguments), patch.object(cli, "request", return_value=snapshot()), \
                 patch.object(cli, "build_development") as build, patch.object(cli, "upload_and_queue") as upload:
                code, _, _ = self.invoke(arguments)
                self.assertEqual(code, 1)
                build.assert_not_called()
                upload.assert_not_called()

    def test_dry_run_selects_devices_without_writing(self):
        with patch.object(cli, "request", return_value=snapshot()), patch.object(cli, "build_development") as build, \
             patch.object(cli, "upload_and_queue") as upload:
            code, output, _ = self.invoke(["flash", "--type", "sensor", "--dry-run"])
        self.assertEqual(code, 0)
        self.assertIn("E857", output)
        self.assertIn("EC52", output)
        self.assertNotIn("59A4", output)
        build.assert_not_called()
        upload.assert_not_called()

    def test_build_types_and_envs_need_no_device_network(self):
        with patch.object(cli, "build_development") as build, patch.object(cli, "request") as request:
            code, _, _ = self.invoke(["build", "--type", "screen,bridge"])
            self.assertEqual(code, 0)
            self.assertEqual(set(build.call_args.args[0]), set(bundles.ENVS) - {"sensor", "arbiter_mesh", "arbiter_crowpanel7"})
            request.assert_not_called()

    def test_ids_build_shared_environment_only_once(self):
        with patch.object(cli, "request", return_value=snapshot()), patch.object(cli, "build_development") as build:
            code, _, _ = self.invoke(["build", "--id", "E857,EC52"])
        self.assertEqual(code, 0)
        build.assert_called_once_with(["sensor"])

    def test_development_builds_get_distinct_embedded_identities(self):
        with patch.object(bundles, "build_bundle") as build:
            cli.build_development(["sensor"])
            cli.build_development(["sensor"])
        first, second = build.call_args_list
        self.assertNotEqual(first.args[2]["sensor"], second.args[2]["sensor"])
        self.assertEqual(len(first.args[2]["sensor"]), 64)
        self.assertTrue(first.args[1].startswith("dev-"))

    def test_failed_build_cannot_upload(self):
        with patch.object(cli, "request", return_value=snapshot()), \
             patch.object(cli, "build_development", side_effect=subprocess.CalledProcessError(1, "pio")), \
             patch.object(cli, "upload_and_queue") as upload:
            code, _, _ = self.invoke(["flash", "--id", "E857"])
        self.assertEqual(code, 1)
        upload.assert_not_called()

    def test_existing_manifest_skips_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path, _ = image_fixture(directory)
            with patch.object(cli, "request", return_value=snapshot()), patch.object(cli, "build_development") as build, \
                 patch.object(cli, "upload_and_queue") as upload:
                code, _, _ = self.invoke(["flash", "--manifest", str(path), "--id", "E857", "--no-wait"])
            self.assertEqual(code, 0)
            build.assert_not_called()
            self.assertFalse(upload.call_args.kwargs["wait"])

    def test_corrupt_bundle_is_rejected_before_any_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            path, manifest = image_fixture(directory)
            (path.parent / manifest["firmware"]["sensor"]["asset"]).write_bytes(b"bad")
            with patch.object(cli, "request") as request, self.assertRaises(ValueError):
                cli.upload_and_queue("http://test", path, manifest, [{"id": "E857", "env": "sensor"}])
            request.assert_not_called()

    def test_fresh_scan_json_is_still_only_json(self):
        job = {"id": "scan1", "state": "completed", "devices": {}}
        def request(url, path="", body=None):
            return {"job": job} if path == "/scan" else snapshot()
        with patch.object(cli, "request", side_effect=request):
            code, output, _ = self.invoke(["list", "--scan", "--json"])
        self.assertEqual(code, 0)
        self.assertEqual(len(json.loads(output)), 5)

    def test_url_override_before_or_after_command(self):
        for args in (["--url", "http://unit", "list", "--json"], ["list", "--json", "--url", "http://unit"]):
            with patch.object(cli, "request", return_value=snapshot()) as request:
                self.assertEqual(self.invoke(args)[0], 0)
                self.assertEqual(request.call_args.args[0], "http://unit")

    def test_usb_refuses_known_chip_mismatch_before_build(self):
        row = {"id": "E857", "env": "sensor", "type": "sensor", "cls": "esp32", "port": "/dev/test"}
        with patch.object(cli, "usb_devices", return_value=[row]), patch.object(cli, "build_development") as build:
            code, _, error = self.invoke(["flash", "--usb", "--type", "sensor"])
        self.assertEqual(code, 1)
        self.assertIn("s3_4mb", error)
        build.assert_not_called()

    def test_explicit_usb_port_and_env_can_provision_blank_board(self):
        row = {"id": "?", "env": "?", "type": "unknown", "cls": "esp32", "port": "/dev/test", "source": "silent"}
        with patch.object(cli, "usb_devices", return_value=[row]), patch.object(cli, "build_development", return_value=(Path('/tmp/x'), {})) as build, \
             patch.object(cli, "flash_usb") as flash, patch.object(cli, "request") as request:
            code, _, _ = self.invoke(["flash", "--usb", "--port", "/dev/test", "--env", "bridge_esp32"])
        self.assertEqual(code, 0)
        build.assert_called_once_with(["bridge_esp32"])
        flash.assert_called_once()
        request.assert_not_called()

    def test_usb_upload_reuses_built_image_and_verifies_reboot(self):
        row = {"id": "E857", "env": "sensor", "port": "/dev/test"}
        manifest = {"firmware": {"sensor": {"build_hash": "a" * 64}}}
        with patch.object(bundles, "pio_path", return_value="pio"), patch.object(cli.subprocess, "run") as run, \
             patch.object(cli, "usb_devices", return_value=[dict(row, sha256="a" * 64)]), contextlib.redirect_stdout(io.StringIO()):
            cli.flash_usb([row], manifest)
        self.assertIn("nobuild", run.call_args.args[0])
        self.assertIn("upload", run.call_args.args[0])
        self.assertEqual(run.call_args.kwargs["env"]["PULLEYS_FIRMWARE_BUILD_HASH"], "a" * 64)

    def test_release_build_and_publish_share_existing_release_workflow(self):
        with patch.object(bundles, "release_bundle") as release:
            self.assertEqual(self.invoke(["release", "--tag", "v0.3.0"])[0], 0)
            release.assert_called_with("v0.3.0", publish=False)
            self.assertEqual(self.invoke(["release", "--publish"])[0], 0)
            release.assert_called_with(None, publish=True)

    def test_release_requires_clean_source_but_dev_build_does_not(self):
        with patch.object(bundles, "pio_path", return_value="pio"), \
             patch.object(bundles.subprocess, "check_output", return_value=" M src/sensor/main.cpp\n"), \
             self.assertRaisesRegex(RuntimeError, "uncommitted development"):
            bundles.release_bundle("v0.3.0")
        with patch.object(bundles.subprocess, "check_output", side_effect=AssertionError("git must not gate dev builds")), \
             patch.object(bundles, "build_bundle"):
            cli.build_development(["sensor"])

    def test_bundle_builds_each_env_once_and_checksums_exact_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def compile_image(command, **kwargs):
                env = command[command.index("-e") + 1]
                path = root / ".pio/build" / env / "firmware.bin"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(kwargs["env"]["PULLEYS_FIRMWARE_BUILD_HASH"].encode())
            with patch.object(bundles, "ROOT", root), patch.object(bundles.subprocess, "run", side_effect=compile_image) as run, \
                 contextlib.redirect_stdout(io.StringIO()):
                path, manifest = bundles.build_bundle(["sensor", "sensor"], "dev-test", {"sensor": "a" * 64}, root / "out", pio="pio")
            self.assertEqual(run.call_count, 1)
            image = manifest["firmware"]["sensor"]
            self.assertEqual(image["sha256"], hashlib.sha256(b"a" * 64).hexdigest())
            self.assertEqual(json.loads(path.read_text()), manifest)

    def test_stale_pio_artifact_cannot_be_packaged_as_a_new_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stale = root / ".pio/build/sensor/firmware.bin"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old-firmware")
            with patch.object(bundles, "ROOT", root), patch.object(bundles.subprocess, "run"), \
                 contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, "stale firmware"):
                bundles.build_bundle(["sensor"], "dev-test", {"sensor": "a" * 64}, root / "out", pio="pio")
            self.assertFalse((root / "out/manifest.json").exists())

    def test_cli_uploads_real_manifest_and_only_selected_ids_to_http_api(self):
        sys.path.insert(0, str(cli.ROOT / "overlord"))
        from control import ControlServer
        from firmware_manager import FirmwareManager
        class Serial:
            connected = True
            def write_line(self, line): return True
        with tempfile.TemporaryDirectory() as directory:
            path, manifest = image_fixture(directory)
            manager = FirmwareManager(Serial(), cache=Path(directory) / "cache")
            for device in ("E857", "EC52"):
                manager.handle_line(f"PULLEYS-CENSUS id={device} env=sensor sha256=unknown")
            server = ControlServer(lambda *args: (404, {}), port=0, firmware=manager)
            server.start()
            try:
                url = f"http://{server.address[0]}:{server.address[1]}"
                with patch.object(cli, "build_development", return_value=(path, manifest)) as build:
                    code, _, errors = self.invoke(["flash", "--url", url, "--id", "E857,EC52", "--no-wait"])
                self.assertEqual(code, 0, errors)
                build.assert_called_once_with(["sensor"])
                state = manager.snapshot()
                self.assertEqual(state["jobs"][0]["device_ids"], ["E857", "EC52"])
                self.assertTrue(state["releases"][0]["ready"])
            finally:
                server.stop()
                manager.stop()

    @unittest.skipUnless(importlib.util.find_spec("serial"), "pyserial not installed")
    def test_usb_version_probe_ignores_neighbor_census_and_keeps_legacy_fields(self):
        from tools import identify
        class Port:
            def __init__(self):
                self.chunks = [b"PULLEYS-ID class=esp32 role=SCREEN env=screen id=ABCD build=Nov-7\n",
                               ("PULLEYS-CENSUS id=BBBB self=0 sha256=" + "b" * 64 + "\n" +
                                "PULLEYS-CENSUS id=ABCD self=1 sha256=" + "a" * 64 + "\n").encode()]
                self.writes = []
            def open(self): pass
            def reset_input_buffer(self): pass
            def read(self, size): return self.chunks.pop(0) if self.chunks else b""
            def write(self, data): self.writes.append(data)
            def flush(self): pass
            def close(self): pass
        port = Port(); rows = []
        with patch.object(identify.serial, "Serial", return_value=port):
            identify.probe("/dev/test", rows, versions=True)
        self.assertEqual(rows[0]["sha256"], "a" * 64)
        self.assertEqual(rows[0]["build"], "Nov-7")
        self.assertEqual(port.writes.count(b"CENSUS 500\n"), 1)
        self.assertEqual(rows[0]["source"], "id")


if __name__ == "__main__":
    unittest.main()
