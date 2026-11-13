"""Pausing, cancelling, reordering and editing the Overlord's firmware queue."""
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
from test_firmware_dashboard import fixture


class Serial:
    """The bridge link. Records what is sent; the device never answers."""
    connected = True

    def __init__(self):
        self.sent = []

    def write_line(self, line):
        self.sent.append(line)
        return True


def wait_for(condition, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(.01)
    return False


class QueueBase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.serial = Serial()
        self.manager = self.make_manager()
        for node in ("59A4", "CC6B", "28A2"):
            self.manager.handle_line(f"PULLEYS-CENSUS id={node} env=screen sha256=unknown name=X-{node} build=old")
        manifest, data = fixture()
        release = self.manager.register_manifest(manifest)
        self.manager.upload_image(release["id"], "screen", data)
        self.release = release["id"]
        self.build_hash = manifest["firmware"]["screen"]["build_hash"]

    def make_manager(self):
        return FirmwareManager(self.serial, cache=self.directory.name)

    def tearDown(self):
        self.manager.stop()
        self.directory.cleanup()

    def job(self, job_id):
        return next(row for row in self.manager.snapshot()["jobs"] if row["id"] == job_id)

    def state(self, job_id):
        return self.job(job_id)["state"]

    def fresh(self, *nodes, current=()):
        """Make discover() answer at once with these nodes; `current` ones already run the new build."""
        now = time.time()
        rows = {node: {"env": "screen", "sha256": self.build_hash if node in current else "unknown"} for node in nodes}
        for node in nodes:
            self.manager.transport._last_seen[node] = now
        return patch.object(self.manager.transport, "discover", return_value=rows)


class PauseAndOrderTests(QueueBase):
    def ran(self):
        order = []
        def update(release, ids, env):
            order.append(ids)
            return {ids[0]: {"state": "confirmed"}}
        return order, patch.object(self.manager.transport, "update_release", side_effect=update)

    def test_a_paused_queue_starts_nothing_until_resumed_and_keeps_its_order(self):
        order, patched = self.ran()
        with patched:
            self.manager.pause()
            first = self.manager.enqueue("update", self.release, ["59A4"])
            second = self.manager.enqueue("update", self.release, ["CC6B"])
            self.manager.start()
            time.sleep(.3)
            self.assertEqual(order, [])
            self.assertEqual(self.state(first["id"]), "queued")
            self.assertTrue(self.manager.snapshot()["paused"])
            self.assertEqual(self.manager.snapshot()["queue"], [first["id"], second["id"]])
            self.manager.resume()
            self.assertTrue(wait_for(lambda: len(order) == 2))
        self.assertEqual(order, [["59A4"], ["CC6B"]])
        self.assertFalse(self.manager.snapshot()["paused"])

    def test_move_changes_what_runs_next(self):
        order, patched = self.ran()
        with patched:
            self.manager.pause()
            a = self.manager.enqueue("update", self.release, ["59A4"])
            b = self.manager.enqueue("update", self.release, ["CC6B"])
            c = self.manager.enqueue("update", self.release, ["28A2"])
            self.assertEqual(self.manager.move(c["id"], "top")["pending"][0]["id"], c["id"])
            self.assertEqual([row["id"] for row in self.manager.move(a["id"], "bottom")["pending"]], [c["id"], b["id"], a["id"]])
            self.assertEqual([row["id"] for row in self.manager.move(a["id"], 1)["pending"]], [c["id"], a["id"], b["id"]])
            with self.assertRaisesRegex(ValueError, "position"):
                self.manager.move(a["id"], "sideways")
            self.manager.start()
            self.manager.resume()
            self.assertTrue(wait_for(lambda: len(order) == 3))
        self.assertEqual(order, [["28A2"], ["59A4"], ["CC6B"]])

    def test_only_a_waiting_job_can_be_moved(self):
        order, patched = self.ran()
        with patched:
            job = self.manager.enqueue("update", self.release, ["59A4"])
            self.manager.start()
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "completed"))
        with self.assertRaisesRegex(ValueError, "waiting"):
            self.manager.move(job["id"], 0)

    def test_pause_survives_a_restart_until_resumed(self):
        self.manager.pause()
        again = self.make_manager()
        self.addCleanup(again.stop)
        self.assertTrue(again.snapshot()["paused"])
        again.resume()
        self.assertFalse(self.make_manager().snapshot()["paused"])

    def test_pausing_holds_a_running_job_before_its_next_device(self):
        # Two devices already on the new build skip instantly, so the hold shows between them.
        with self.fresh("59A4", "CC6B", current=("59A4", "CC6B")):
            self.manager.pause()
            job = self.manager.enqueue("update", self.release, ["59A4", "CC6B"])
            self.manager.resume()
            self.manager.transport.set_paused(True)   # pause lands while the job is running
            self.manager.start()
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "running"))
            time.sleep(.4)
            self.assertEqual(self.state(job["id"]), "running", "held, not finished")
            self.assertIn("paused", " ".join(str(v.get("message", "")) for v in self.job(job["id"])["devices"].values()))
            self.manager.transport.set_paused(False)
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "completed"))
        self.assertEqual({v["state"] for v in self.job(job["id"])["devices"].values()}, {"skipped"})


class CancelTests(QueueBase):
    def test_cancelling_a_waiting_job_drops_it_without_running(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, ["59A4"])
        cancelled = self.manager.cancel(job["id"])
        self.assertEqual(cancelled["state"], "cancelled")
        self.assertEqual(self.manager.snapshot()["queue"], [])
        with self.assertRaisesRegex(ValueError, "already"):
            self.manager.cancel(job["id"])

    def test_cancelling_a_running_transfer_stops_it_promptly_and_refuses_more_chunks(self):
        with self.fresh("59A4", "CC6B"):
            job = self.manager.enqueue("update", self.release, ["59A4", "CC6B"])
            self.manager.start()
            # The device never reports, so it sits in the download wait: that is what cancel must interrupt.
            self.assertTrue(wait_for(lambda: any(line.startswith("FW-OFFER id=") for line in self.serial.sent)))
            started = time.monotonic()
            self.manager.cancel(job["id"])
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "cancelled", 5))
            self.assertLess(time.monotonic() - started, 5, "must not wait out the 3 minute transfer timeout")
        result = self.job(job["id"])
        self.assertEqual(result["error"], "Cancelled")
        states = {node: row["state"] for node, row in result["devices"].items()}
        self.assertEqual(sorted(states.values()), ["cancelled", "cancelled"])
        self.assertEqual(sum(line.startswith("FW-OFFER") for line in self.serial.sent), 1, "the second device was never offered")
        digest = next(iter(self.manager.transport._assets))
        self.serial.sent.clear()
        self.manager.transport.handle_line(f"FW-READ sha256={digest} offset=0 length=64")
        self.assertTrue(any("FW-ERROR" in line and "reason=cancelled" in line for line in self.serial.sent))
        self.assertFalse(any(line.startswith("FW-DATA") for line in self.serial.sent))

    def test_the_next_job_runs_normally_after_a_cancel(self):
        with self.fresh("59A4", "CC6B", current=("CC6B",)):
            slow = self.manager.enqueue("update", self.release, ["59A4"])
            after = self.manager.enqueue("update", self.release, ["CC6B"])
            self.manager.start()
            self.assertTrue(wait_for(lambda: any(line.startswith("FW-OFFER") for line in self.serial.sent)))
            self.manager.cancel(slow["id"])
            self.assertTrue(wait_for(lambda: self.state(after["id"]) == "completed", 10))
        self.assertEqual(self.state(slow["id"]), "cancelled")

    def test_a_cancelled_job_can_be_retried_for_what_did_not_finish(self):
        with self.fresh("59A4", "CC6B", current=("59A4",)):
            job = self.manager.enqueue("update", self.release, ["59A4", "CC6B"])
            self.manager.start()
            self.assertTrue(wait_for(lambda: any(line.startswith("FW-OFFER id=CC6B") for line in self.serial.sent)))
            self.manager.cancel(job["id"])
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "cancelled", 5))
            retry = self.manager.retry(job["id"])
        self.assertEqual(retry["device_ids"], ["CC6B"], "the skipped-as-current device is not redone")

    def test_clear_queue_cancels_the_waiting_jobs_and_optionally_the_running_one(self):
        self.manager.pause()
        jobs = [self.manager.enqueue("update", self.release, [node]) for node in ("59A4", "CC6B")]
        view = self.manager.clear_queue()
        self.assertEqual(view["pending"], [])
        self.assertEqual([self.state(job["id"]) for job in jobs], ["cancelled", "cancelled"])

    def test_delete_cancels_an_active_job_and_removes_a_finished_one(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, ["59A4"])
        self.assertEqual(self.manager.remove_job(job["id"])["cancelled"]["state"], "cancelled")
        self.assertEqual(self.manager.remove_job(job["id"]), {"removed": job["id"]})
        with self.assertRaises(KeyError):
            self.manager.remove_job(job["id"])


class EditTests(QueueBase):
    def test_devices_can_be_added_to_and_removed_from_a_waiting_job(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, ["59A4", "CC6B"])
        self.assertEqual(self.manager.edit_devices(job["id"], add=["28a2"])["device_ids"], ["59A4", "CC6B", "28A2"])
        self.assertEqual(self.manager.edit_devices(job["id"], remove=["CC6B"])["device_ids"], ["59A4", "28A2"])
        with self.assertRaisesRegex(ValueError, "not in this job"):
            self.manager.edit_devices(job["id"], remove=["CC6B"])
        with self.assertRaisesRegex(ValueError, "not been discovered"):
            self.manager.edit_devices(job["id"], add=["FFFF"])
        self.manager.edit_devices(job["id"], remove=["59A4"])
        with self.assertRaisesRegex(ValueError, "last device"):
            self.manager.edit_devices(job["id"], remove=["28A2"])

    def test_an_every_outdated_device_job_takes_exclusions_not_additions(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, None)
        with self.assertRaisesRegex(ValueError, "every outdated"):
            self.manager.edit_devices(job["id"], add=["59A4"])
        self.assertEqual(self.manager.edit_devices(job["id"], remove=["CC6B"])["exclude"], ["CC6B"])

    def test_a_running_job_can_drop_a_device_it_has_not_reached(self):
        with self.fresh("59A4", "CC6B", current=("59A4", "CC6B")):
            self.manager.transport.set_paused(True)
            job = self.manager.enqueue("update", self.release, ["59A4", "CC6B"])
            self.manager.start()
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "running"))
            with self.assertRaisesRegex(ValueError, "devices can only be added"):
                self.manager.edit_devices(job["id"], add=["28A2"])
            self.manager.edit_devices(job["id"], remove=["CC6B"])
            self.manager.transport.set_paused(False)
            self.assertTrue(wait_for(lambda: self.state(job["id"]) == "completed"))
        devices = self.job(job["id"])["devices"]
        self.assertEqual(devices["CC6B"], {"state": "skipped", "message": "removed from this job"})
        self.assertEqual(devices["59A4"]["message"], "already current")

    def test_delete_release_frees_its_images_unless_a_job_needs_it(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, ["59A4"])
        with self.assertRaisesRegex(ValueError, "uses this release"):
            self.manager.delete_release(self.release)
        self.manager.cancel(job["id"])
        images = [p for p in Path(self.directory.name).rglob("*.bin")]
        self.assertTrue(images)
        result = self.manager.delete_release(self.release)
        self.assertGreater(result["freed_bytes"], 0)
        self.assertEqual([p for p in Path(self.directory.name).rglob("*.bin")], [])
        self.assertEqual(self.manager.snapshot()["releases"], [])
        with self.assertRaises(KeyError):
            self.manager.delete_release(self.release)

    def test_logs_can_be_followed_by_sequence_and_filtered_by_job(self):
        self.manager.pause()
        job = self.manager.enqueue("update", self.release, ["59A4"])
        self.manager.cancel(job["id"])
        everything = self.manager.logs()
        self.assertTrue(everything["logs"])
        sequence = everything["logs"][0]["sequence"]
        newer = self.manager.logs(since=sequence)
        self.assertTrue(all(row["sequence"] > sequence for row in newer["logs"]))
        self.assertEqual(newer["last_sequence"], everything["last_sequence"])
        mine = self.manager.logs(job_id=job["id"])["logs"]
        self.assertTrue(mine and all(row["job_id"] == job["id"] for row in mine))
        self.assertEqual(self.manager.logs(limit=1)["logs"][-1]["sequence"], everything["last_sequence"])


class HTTPTests(QueueBase):
    def setUp(self):
        super().setUp()
        self.server = ControlServer(lambda *a: (404, {}), host="127.0.0.1", port=0, firmware=self.manager)
        self.server.start()
        self.addCleanup(self.server.stop)

    def call(self, method, path, body=None):
        connection = http.client.HTTPConnection(*self.server.address, timeout=10)
        connection.request(method, "/api/firmware" + path, body=json.dumps(body).encode() if body is not None else None,
                           headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        data = json.loads(response.read() or b"{}")
        connection.close()
        return response.status, data

    def test_the_whole_queue_workflow_over_http(self):
        status, view = self.call("POST", "/queue/pause")
        self.assertEqual((status, view["paused"]), (200, True))
        _, first = self.call("POST", "/updates", {"release_id": self.release, "device_ids": ["59A4"]})
        _, second = self.call("POST", "/updates", {"release_id": self.release, "device_ids": ["CC6B"]})
        first, second = first["job"]["id"], second["job"]["id"]
        _, queue = self.call("GET", "/queue")
        self.assertEqual([row["id"] for row in queue["pending"]], [first, second])
        self.assertEqual(self.call("POST", f"/jobs/{second}/move", {"position": "top"})[1]["pending"][0]["id"], second)
        self.assertEqual(self.call("POST", f"/jobs/{second}/devices", {"add": ["28A2"]})[1]["job"]["device_ids"], ["CC6B", "28A2"])
        self.assertEqual(self.call("DELETE", f"/jobs/{second}/devices/28A2")[1]["job"]["device_ids"], ["CC6B"])
        self.assertEqual(self.call("POST", f"/jobs/{first}/cancel")[1]["job"]["state"], "cancelled")
        self.assertEqual(self.call("DELETE", f"/jobs/{second}")[1]["cancelled"]["state"], "cancelled")
        self.assertEqual(self.call("DELETE", f"/jobs/{second}")[1], {"removed": second})
        self.assertEqual(self.call("POST", "/queue/resume")[1]["paused"], False)
        self.assertEqual(self.call("GET", "/")[1]["paused"], False)

    def test_clear_logs_and_release_deletion_over_http(self):
        self.call("POST", "/queue/pause")
        self.call("POST", "/updates", {"release_id": self.release, "device_ids": ["59A4"]})
        self.assertEqual(self.call("POST", "/queue/clear", {})[1]["pending"], [])
        status, logs = self.call("GET", "/logs?since=0&limit=50")
        self.assertEqual(status, 200)
        self.assertTrue(logs["logs"])
        self.assertEqual(self.call("GET", f"/logs?since={logs['last_sequence']}")[1]["logs"], [])
        status, deleted = self.call("DELETE", f"/releases/{self.release}")
        self.assertEqual((status, deleted["deleted"]), (200, self.release))

    def test_unknown_ids_are_404_and_bad_requests_are_400(self):
        self.assertEqual(self.call("POST", "/jobs/nope/cancel")[0], 404)
        self.assertEqual(self.call("DELETE", "/jobs/nope")[0], 404)
        self.assertEqual(self.call("DELETE", "/releases/nope")[0], 404)
        self.call("POST", "/queue/pause")
        job = self.call("POST", "/updates", {"release_id": self.release, "device_ids": ["59A4"]})[1]["job"]["id"]
        self.assertEqual(self.call("POST", f"/jobs/{job}/move", {"position": "sideways"})[0], 400)
        self.assertEqual(self.call("POST", f"/jobs/{job}/devices", {})[0], 400)
        self.assertEqual(self.call("POST", f"/jobs/{job}/devices", {"add": "59A4"})[0], 400)


if __name__ == "__main__":
    unittest.main()
