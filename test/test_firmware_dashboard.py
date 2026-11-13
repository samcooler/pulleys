"""HTTP, upload integrity, persistence and shared-worker regression coverage."""
import hashlib
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "overlord"))
from control import ControlServer
from firmware_manager import FirmwareManager


class Serial:
    connected = True
    def write_line(self, line):
        return True


def fixture(env="screen", chip=0, declared=None):
    identity = "a" * 64
    data = bytearray(128)
    data[0] = 0xe9
    data[12:14] = chip.to_bytes(2, "little")
    data[32:96] = identity.encode()
    data = bytes(data)
    image = {"asset": env + "-firmware.bin", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
             "md5": hashlib.md5(data).hexdigest(), "build_hash": declared or identity}
    return {"schema": 1, "tag": "1763000000", "built_at": 1763000000, "firmware": {env: image}}, data


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.manager = FirmwareManager(Serial(), cache=self.directory.name)
        self.manager.handle_line("PULLEYS-CENSUS id=59A4 env=screen sha256=unknown name=Gauge build=old")
        self.server = None

    def tearDown(self):
        if self.server:
            self.server.stop()
        self.manager.stop()
        self.directory.cleanup()

    def uploaded(self):
        manifest, data = fixture()
        release = self.manager.register_manifest(manifest)
        self.manager.upload_image(release["id"], "screen", data)
        return release["id"]

    def test_upload_persistence_and_interrupted_jobs(self):
        manifest, data = fixture()
        release = self.manager.register_manifest(manifest)
        self.assertFalse(release["ready"])
        with self.assertRaisesRegex(ValueError, "upload an image"):
            self.manager.enqueue("update", release["id"], ["59A4"])
        ready = self.manager.upload_image(release["id"], "screen", data)
        self.assertTrue(ready["ready"])
        job = self.manager.enqueue("update", release["id"], ["59a4"])
        restored = FirmwareManager(Serial(), cache=self.directory.name)
        snapshot = restored.snapshot()
        self.assertTrue(snapshot["releases"][0]["ready"])
        self.assertEqual(snapshot["releases"][0]["built_at"], 1763000000)   # survives a restart
        self.assertEqual(snapshot["jobs"][0]["id"], job["id"])
        self.assertEqual(snapshot["jobs"][0]["state"], "interrupted")
        self.assertEqual(snapshot["devices"][0]["id"], "59A4")

    def test_online_means_heard_within_a_few_scans_and_widens_during_a_job(self):
        node = lambda: next(row for row in self.manager.snapshot()["devices"] if row["id"] == "59A4")
        self.manager.transport._last_seen["59A4"] = time.time() - 5
        self.assertTrue(node()["online"])
        self.manager.transport._last_seen["59A4"] = time.time() - 30
        self.assertFalse(node()["online"])
        self.manager._active = "job"      # no scans run during a job, so be patient
        self.assertTrue(node()["online"])

    def test_scan_asks_for_one_census_and_does_nothing_without_a_bridge(self):
        sent = []
        class Quiet:
            connected = True
            def write_line(self, line): sent.append(line); return True
        transport = FirmwareManager(Quiet(), cache=self.directory.name).transport
        transport.scan(spread_ms=100)
        self.assertEqual(sent, ["FW-CENSUS spread=100"])
        Quiet.connected = False
        transport.scan(spread_ms=100)
        self.assertEqual(len(sent), 1)

    def test_built_at_must_be_a_unix_timestamp(self):
        manifest, _ = fixture()
        for bad in ("2025-11-12", 0, -5, 1.5, True):
            with self.assertRaisesRegex(ValueError, "built_at"):
                self.manager.register_manifest(dict(manifest, built_at=bad))

    def test_rejects_corruption_wrong_chip_and_false_identity(self):
        for env, chip, identity, message in [("sensor", 0, None, "chip type"),
                                              ("screen", 0, "b" * 64, "build identity")]:
            with self.subTest(env=env, identity=identity):
                manifest, data = fixture(env, chip, identity)
                release = self.manager.register_manifest(manifest)
                with self.assertRaisesRegex(ValueError, message):
                    self.manager.upload_image(release["id"], env, data)
        manifest, data = fixture()
        release = self.manager.register_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.manager.upload_image(release["id"], "screen", data[:-1] + b"x")
        self.manager.upload_image(release["id"], "screen", data)
        manifest["firmware"]["screen"]["build_hash"] = "c" * 64
        with self.assertRaisesRegex(ValueError, "build identity"):
            self.manager.register_manifest(manifest)

    def test_rejects_unknown_devices_and_environment_mismatch(self):
        release_id = self.uploaded()
        for ids, env in [(["FFFF"], None), (["59A4"], "sensor"), (["59A4; reboot"], None), ([], None)]:
            with self.subTest(ids=ids, env=env), self.assertRaises(ValueError):
                self.manager.enqueue("update", release_id, ids, env)

    def test_manual_jobs_use_one_worker(self):
        release_id = self.uploaded()
        counts = {"active": 0, "maximum": 0, "calls": 0}
        lock = threading.Lock()
        def update(*args):
            with lock:
                counts["active"] += 1
                counts["maximum"] = max(counts["maximum"], counts["active"])
            time.sleep(.03)
            self.manager._event("device", {"node_id": "59A4", "state": "confirmed"})
            with lock:
                counts["active"] -= 1
                counts["calls"] += 1
            return {"59A4": {"state": "confirmed"}}
        with patch.object(self.manager.transport, "update_release", side_effect=update):
            self.manager.start()
            self.manager.enqueue("update", release_id, ["59A4"])
            self.manager.enqueue("update", release_id, ["59A4"])
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                jobs = self.manager.snapshot()["jobs"]
                if len(jobs) == 2 and all(job["state"] == "completed" for job in jobs):
                    break
                time.sleep(.01)
            self.assertEqual(counts, {"active": 0, "maximum": 1, "calls": 2})
            self.assertFalse(any(job["kind"] == "startup" for job in self.manager.snapshot()["jobs"]), "start() must not queue anything")
            self.assertTrue(all(job["state"] == "completed" for job in self.manager.snapshot()["jobs"]))

    def test_retry_only_failed_devices(self):
        release_id = self.uploaded()
        self.manager.handle_line("PULLEYS-CENSUS id=CC6B env=screen sha256=unknown")
        job = self.manager.enqueue("update", release_id, ["59A4", "CC6B"])
        row = self.manager._jobs[0]
        row.update(state="failed", devices={"59A4": {"state": "confirmed"}, "CC6B": {"state": "failed"}})
        retried = self.manager.retry(job["id"])
        self.assertEqual(retried["device_ids"], ["CC6B"])

    def test_progress_counts_retries_once_and_ack_is_not_confirmation(self):
        release_id = self.uploaded()
        job = self.manager.enqueue("update", release_id, ["59A4"])
        self.manager._active = job["id"]
        self.manager._event("device", {"node_id": "59A4", "state": "offering", "total": 128, "bytes": 0})
        for value in (96, 96, 64):
            self.manager._event("progress", {"node_id": "59A4", "bytes": value})
        self.manager.handle_line("FW-STATUS id=59A4 state=ok")
        device = self.manager.snapshot()["jobs"][0]["devices"]["59A4"]
        self.assertEqual(device["bytes"], 96)
        self.assertEqual(device["state"], "verifying")

    def http(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(*self.server.address, timeout=5)
        if isinstance(body, dict):
            body = json.dumps(body)
            headers = {"Content-Type": "application/json"}
        connection.request(method, path, body, headers or {})
        response = connection.getresponse()
        data = response.read()
        content_type = response.getheader("Content-Type")
        status = response.status
        connection.close()
        return status, json.loads(data) if "application/json" in content_type else data

    def test_real_http_upload_and_queue_and_existing_routes(self):
        self.server = ControlServer(lambda method, parts: (200, {"old_control": True}),
                                    host="127.0.0.1", port=0, firmware=self.manager)
        self.server.start()
        self.assertIn(b"Firmware", self.http("GET", "/")[1])
        self.assertEqual(self.http("GET", "/dashboard.js")[0], 200)
        self.assertEqual(self.http("GET", "/status")[1], {"old_control": True})
        manifest, data = fixture()
        status, response = self.http("POST", "/api/firmware/releases", manifest)
        self.assertEqual(status, 201)
        release_id = response["release"]["id"]
        status, response = self.http("PUT", f"/api/firmware/releases/{release_id}/screen", data)
        self.assertEqual(status, 200)
        self.assertTrue(response["release"]["ready"])
        status, response = self.http("POST", "/api/firmware/updates", {"release_id": release_id, "device_ids": ["59A4"]})
        self.assertEqual(status, 202)
        self.assertEqual(response["job"]["state"], "queued")
        self.assertEqual(self.http("GET", "/api/firmware/jobs/" + response["job"]["id"])[0], 200)
        self.assertEqual(self.http("POST", "/api/firmware/updates", {"release_id": release_id})[0], 400)
        self.assertEqual(self.http("POST", "/api/firmware/releases", b"not json")[0], 400)
        self.assertEqual(self.http("GET", "/api/firmware/jobs/missing")[0], 404)
        self.assertEqual(self.http("PUT", f"/api/firmware/releases/{release_id}/screen", b"",
                                   {"Content-Length": str(8 * 1024 * 1024)})[0], 400)
        snapshot = self.http("GET", "/api/firmware")[1]
        self.assertNotIn("assets", snapshot)
        self.assertEqual(snapshot["devices"][0]["id"], "59A4")


if __name__ == "__main__":
    unittest.main()
