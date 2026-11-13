"""Durable firmware catalog and one queue shared by HTTP and scripts. Firmware is pushed in; nothing is fetched."""
import copy
import hashlib
import json
from pathlib import Path
import re
import threading
import time
import uuid

from firmware import ACTIVE_ENVS, OPTIONAL_ENVS, CACHE, FirmwareUpdater

ENVS = ACTIVE_ENVS | OPTIONAL_ENVS
MAX_IMAGE = 7 * 1024 * 1024
TERMINAL = {"completed", "failed", "interrupted", "cancelled"}


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or type(manifest.get("schema")) is not int or manifest.get("schema") != 1:
        raise ValueError("manifest must use release schema 1")
    if not isinstance(manifest.get("tag"), str) or not 1 <= len(manifest["tag"]) <= 128:
        raise ValueError("manifest needs a tag")
    # Unix time the build batch was triggered; older manifests have only a tag.
    if "built_at" in manifest and (type(manifest["built_at"]) is not int or manifest["built_at"] <= 0):
        raise ValueError("built_at must be a Unix timestamp")
    table = manifest.get("firmware")
    if not isinstance(table, dict) or not table or not set(table) <= ENVS:
        raise ValueError("manifest must contain supported firmware environments")
    for env, item in table.items():
        if not isinstance(item, dict):
            raise ValueError(f"invalid metadata for {env}")
        for key, length in (("sha256", 64), ("build_hash", 64), ("md5", 32)):
            if not isinstance(item.get(key), str) or not re.fullmatch(f"[0-9a-f]{{{length}}}", item[key]):
                raise ValueError(f"{env}: invalid {key}")
        if type(item.get("size")) is not int or not 24 <= item["size"] <= MAX_IMAGE:
            raise ValueError(f"{env}: invalid image size")
        if not isinstance(item.get("asset"), str) or not re.fullmatch(r"[A-Za-z0-9_.-]+\.bin", item["asset"]):
            raise ValueError(f"{env}: invalid asset filename")
    return copy.deepcopy(manifest)


class FirmwareManager:
    def __init__(self, serial_reader, cache=None):
        self.cache = (Path(cache) if cache is not None else CACHE).expanduser().resolve()
        self.transport = FirmwareUpdater(serial_reader, observer=self._event, cache=self.cache)
        self._lock = self.transport._lock
        # Jobs waiting to run, in the order they will, and whether the queue is held.
        self._pending = []
        self._paused = False
        self._wake = threading.Condition(self._lock)
        self._stop = threading.Event()
        self._thread = None
        self._releases = {}
        self._jobs = []
        self._logs = []
        self._active = None
        self._sequence = 0
        self._last_progress_log = {}
        self._load()

    def _load(self):
        try:
            state = json.loads((self.cache / "dashboard.json").read_text())
            for release in state.get("releases", []):
                if release.get("source") == "github":
                    continue   # from when the Overlord fetched releases itself; it no longer does
                validate_manifest({"schema": 1, "tag": release["tag"], "firmware": release["manifest"],
                                   **({"built_at": release["built_at"]} if release.get("built_at") else {})})
                self._releases[release["id"]] = release
            self._paused = bool(state.get("paused"))   # stays held across a restart until resumed
            self.transport.set_paused(self._paused)
            self._jobs = state.get("jobs", [])[-100:]
            for job in self._jobs:
                if job.get("state") not in TERMINAL:
                    job.update(state="interrupted", error="Overlord restarted before this job completed", finished_at=time.time())
            self._logs = state.get("logs", [])[-500:]
            self._sequence = max((row["sequence"] for row in self._logs), default=0)
            self.transport._inventory = state.get("inventory", {})
            self.transport._last_seen = state.get("last_seen", {})
            for digest, relative in state.get("assets", {}).items():
                path = (self.cache / relative).resolve()
                if path.is_relative_to(self.cache.resolve()) and path.is_file():
                    data = path.read_bytes()
                    if hashlib.sha256(data).hexdigest() == digest:
                        self.transport._assets[digest] = path
        except FileNotFoundError:
            pass
        except (ValueError, KeyError, TypeError, OSError) as exc:
            self._log(f"Could not restore firmware dashboard state: {exc}", level="error")

    def _save(self):
        # Caller holds the same lock used by serial callbacks and snapshots.
        self.cache.mkdir(parents=True, exist_ok=True)
        state = {"releases": list(self._releases.values()), "paused": self._paused,
                 "jobs": self._jobs[-100:], "logs": self._logs[-500:],
                 "inventory": self.transport._inventory, "last_seen": self.transport._last_seen,
                 "assets": {key: str(path.relative_to(self.cache)) for key, path in self.transport._assets.items()}}
        temporary = self.cache / "dashboard.json.tmp"
        temporary.write_text(json.dumps(state))
        temporary.replace(self.cache / "dashboard.json")

    def _log(self, message, node_id=None, level="info", job_id=None):
        with self._lock:
            self._sequence += 1
            self._logs.append({"sequence": self._sequence, "at": time.time(), "message": message,
                               "node_id": node_id, "job_id": job_id or self._active, "level": level})
            self._logs = self._logs[-500:]

    def _event(self, kind, fields):
        with self._lock:
            if not self._active or kind == "inventory":
                return
            job = next(row for row in self._jobs if row["id"] == self._active)
            node_id = fields.get("node_id")
            if not node_id:
                return
            device = job["devices"].setdefault(node_id, {})
            if kind in {"status", "progress"}:
                self.transport._last_seen[node_id] = time.time()
            if kind == "status":
                state = fields.get("state")
                # An install ACK is followed by a separate reboot/hash check.
                stage = {"ready": "connecting", "downloading": "transferring", "ok": "verifying"}.get(state)
                if stage:
                    device["state"] = stage
                    self._log(stage.capitalize(), node_id)
                if state == "failed":
                    self._log(f"Device reported failure: {fields.get('err', 'unknown')}", node_id, "error")
                if fields.get("offset", "").isdigit():
                    device["bytes"] = int(fields["offset"])
            elif kind == "progress":
                # Requests can be retried: count the greatest served offset once.
                device["bytes"] = max(device.get("bytes", 0), fields["bytes"])
                if device.get("state") not in ("verifying", "confirmed"):
                    device["state"] = "transferring"
                mark = device["bytes"] // 65536
                if mark > self._last_progress_log.get(node_id, -1):
                    self._last_progress_log[node_id] = mark
                    self._log(f"Served {device['bytes']:,} of {device.get('total', 0):,} bytes", node_id)
            elif kind == "device":
                if fields.get("state") == "offering":
                    device.pop("error", None)
                    self._last_progress_log[node_id] = -1
                device.update({key: value for key, value in fields.items() if key != "node_id"})
                self._log(fields.get("error") or fields.get("message") or fields.get("state", "Update"),
                          node_id, "error" if fields.get("state") == "failed" else "info")
            if device.get("state") not in ("confirmed", "skipped", "failed"):
                job["current_device"] = node_id

    def handle_line(self, line):
        return self.transport.handle_line(line)

    def start(self):
        with self._lock:
            if self._thread:
                return
            self._thread = threading.Thread(target=self._worker, name="firmware-queue", daemon=True)
            self._thread.start()

    def stop(self):
        self._stop.set()
        with self._wake:
            self._wake.notify_all()
        self.transport.request_cancel()   # let a running update stop waiting
        if self._thread:
            self._thread.join(timeout=2)
        with self._lock:
            self._save()

    def _validate_update(self, release_id, device_ids, env):
        """Check a selection the same way for a new job and for edits to a queued
        one. Returns the device IDs normalised (or None for "every outdated device")."""
        if not isinstance(release_id, str) or release_id not in self._releases:
            raise ValueError("choose an available release")
        if env is not None and (not isinstance(env, str) or env not in ENVS):
            raise ValueError("unsupported firmware environment")
        if device_ids is not None:
            if not isinstance(device_ids, list) or not device_ids or any(
                    not isinstance(value, str) or not re.fullmatch(r"[0-9A-Fa-f]{4}", value) for value in device_ids):
                raise ValueError("device_ids must be a nonempty list of four-digit hex IDs")
            device_ids = list(dict.fromkeys(value.upper() for value in device_ids))
            if any(value not in self.transport._inventory for value in device_ids):
                raise ValueError("a selected device has not been discovered")
        known = [(key, node) for key, node in self.transport._inventory.items()
                 if (device_ids is None or key in device_ids) and (env is None or node.get("env") == env)]
        if not known:
            raise ValueError("no known devices match this selection; scan the mesh first")
        images = self._releases[release_id]["manifest"]
        if device_ids is None:
            known = [(key, node) for key, node in known if node.get("env") in images]
        if not known:
            raise ValueError("no known devices match this release")
        for _, node in known:
            image = images.get(node.get("env"))
            if image is None or image["sha256"] not in self.transport._assets:
                raise ValueError(f"upload an image for {node.get('env', 'unknown')} first")
        if device_ids and env and any(self.transport._inventory[key].get("env") != env for key in device_ids):
            raise ValueError("a selected device does not match env")
        return device_ids

    def enqueue(self, kind, release_id=None, device_ids=None, env=None):
        if kind not in {"scan", "update"}:
            raise ValueError("unsupported job type")
        with self._lock:
            if sum(row["state"] in {"queued", "running"} for row in self._jobs) >= 25:
                raise ValueError("firmware queue is full")
            if kind == "update":
                device_ids = self._validate_update(release_id, device_ids, env)
            job = {"id": uuid.uuid4().hex[:12], "kind": kind, "state": "queued", "created_at": time.time(),
                   "release_id": release_id, "device_ids": device_ids, "env": env, "devices": {}}
            self._jobs.append(job)
            # Never discard a queued/running job when keeping bounded history.
            completed = [row for row in self._jobs if row["state"] in TERMINAL]
            for row in completed[:-75]:
                self._jobs.remove(row)
            self._pending.append(job["id"])
            self._save()
            self._wake.notify_all()
            return copy.deepcopy(job)

    def retry(self, job_id):
        with self._lock:
            job = next((row for row in self._jobs if row["id"] == job_id), None)
            if not job or job["state"] not in {"failed", "interrupted", "cancelled"}:
                raise ValueError("only failed, interrupted or cancelled jobs can be retried")
            if job["kind"] == "update" or job.get("release_id"):
                # Failed devices; for a cancelled job, everything that did not finish.
                unfinished = {"failed"} if job["state"] != "cancelled" else None
                todo = [key for key, value in job["devices"].items()
                        if (value.get("state") in unfinished if unfinished else value.get("state") not in {"confirmed", "skipped"})]
                return self.enqueue("update", job["release_id"], todo or job["device_ids"], job["env"])
            return self.enqueue(job["kind"])

    # ── the queue: pause, cancel, reorder, edit ──────────────────────────────
    def _job(self, job_id):
        job = next((row for row in self._jobs if row["id"] == job_id), None)
        if not job:
            raise KeyError(job_id)
        return job

    def queue_view(self):
        """What is running, what is waiting and in what order, and whether the queue is held."""
        with self._lock:
            def brief(job):
                return {key: job.get(key) for key in ("id", "kind", "state", "release_id", "device_ids", "env", "created_at")}
            return copy.deepcopy({"paused": self._paused,
                                  "running": brief(self._job(self._active)) if self._active else None,
                                  "pending": [brief(self._job(job_id)) for job_id in self._pending]})

    def pause(self):
        """Hold the queue. The device being flashed finishes; nothing new starts, not
        the next device of the running job and not the next job, until resume()."""
        with self._lock:
            if not self._paused:
                self._paused = True
                self.transport.set_paused(True)
                self._log("Queue paused" + ("; the device being flashed will finish first" if self._active else ""))
                self._save()
            return self.queue_view()

    def resume(self):
        with self._wake:
            if self._paused:
                self._paused = False
                self.transport.set_paused(False)
                self._log("Queue resumed")
                self._save()
                self._wake.notify_all()
            return self.queue_view()

    def cancel(self, job_id):
        """Stop a job. A waiting one is dropped; a running one stops serving its
        device and takes no more (the device discards a partial download)."""
        with self._wake:
            job = self._job(job_id)
            if job["state"] == "queued":
                self._pending.remove(job_id)
                job.update(state="cancelled", error="Cancelled before it started", finished_at=time.time())
                self._log("Cancelled a queued job", job_id=job_id)
            elif job["state"] == "running":
                if not job.get("cancel_requested"):
                    job["cancel_requested"] = True
                    self._log("Cancel requested; stopping the transfer")
                    self.transport.request_cancel()
            else:
                raise ValueError(f"job already {job['state']}")
            self._save()
            return copy.deepcopy(job)

    def remove_job(self, job_id):
        """DELETE a job: cancel it if it is waiting or running, drop it from the history if it is over."""
        with self._lock:
            job = self._job(job_id)
            if job["state"] in TERMINAL:
                self._jobs.remove(job)
                self._save()
                return {"removed": job_id}
        return {"cancelled": self.cancel(job_id)}

    def move(self, job_id, position):
        """Reorder a waiting job: 0 runs next, or "top" / "bottom"."""
        with self._wake:
            job = self._job(job_id)
            if job["state"] != "queued":
                raise ValueError("only a waiting job can be moved")
            if position == "top":
                position = 0
            elif position == "bottom":
                position = len(self._pending) - 1
            elif type(position) is not int:
                raise ValueError('position must be a number, "top" or "bottom"')
            self._pending.remove(job_id)
            self._pending.insert(max(0, min(position, len(self._pending))), job_id)
            self._save()
            self._wake.notify_all()
            return self.queue_view()

    def clear_queue(self, include_running=False):
        """Cancel every waiting job; with include_running, the running one too."""
        with self._lock:
            waiting = list(self._pending)
            running = self._active if include_running else None
        for job_id in waiting:
            self.cancel(job_id)
        if running:
            self.cancel(running)
        return self.queue_view()

    def edit_devices(self, job_id, add=(), remove=()):
        """Change which devices a job will flash. A waiting job can gain or lose
        devices; a running job can only lose ones it has not reached yet."""
        with self._lock:
            job = self._job(job_id)
            if job["state"] in TERMINAL or job["kind"] != "update":
                raise ValueError("only a waiting or running update can be edited")
            add = [value.upper() for value in add]
            remove = [value.upper() for value in remove]
            if add:
                if job["state"] != "queued":
                    raise ValueError("devices can only be added to a job that has not started; queue a new job instead")
                if job["device_ids"] is None:
                    raise ValueError("this job targets every outdated device; queue a new job for specific ones")
                self._validate_update(job["release_id"], list(dict.fromkeys(job["device_ids"] + add)), job["env"])
                job["device_ids"] = list(dict.fromkeys(job["device_ids"] + add))
                self._log(f"Added {', '.join(add)}", job_id=job_id)
            for node_id in remove:
                if job["state"] == "running":
                    if job["devices"].get(node_id, {}).get("state") not in (None, "queued"):
                        raise ValueError(f"{node_id} is already being flashed or done; cancel the job to stop it")
                    job.setdefault("exclude", [])
                    if node_id not in job["exclude"]:
                        job["exclude"].append(node_id)
                    self.transport.exclude_device(node_id)
                elif job["device_ids"] is not None:
                    if node_id not in job["device_ids"]:
                        raise ValueError(f"{node_id} is not in this job")
                    if len(job["device_ids"]) == 1:
                        raise ValueError("that is the job's last device; cancel the job instead")
                    job["device_ids"].remove(node_id)
                else:
                    job.setdefault("exclude", [])
                    if node_id not in job["exclude"]:
                        job["exclude"].append(node_id)
                self._log(f"Removed {node_id}", job_id=job_id)
            self._save()
            return copy.deepcopy(job)

    def delete_release(self, release_id):
        """Drop a release and the images only it used, to free the cache."""
        with self._lock:
            if release_id not in self._releases:
                raise KeyError(release_id)
            if any(row["state"] in {"queued", "running"} and row.get("release_id") == release_id for row in self._jobs):
                raise ValueError("a waiting or running job uses this release")
            removed = self._releases.pop(release_id)
            still_used = {image["sha256"] for release in self._releases.values() for image in release["manifest"].values()}
            freed = 0
            for image in removed["manifest"].values():
                digest = image["sha256"]
                path = self.transport._assets.get(digest)
                if digest in still_used or path is None:
                    continue
                self.transport._assets.pop(digest)
                try:
                    if self.cache in Path(path).resolve().parents:   # only ever delete inside the cache
                        freed += Path(path).stat().st_size
                        Path(path).unlink()
                except OSError:
                    pass
            self._log(f"Deleted release {removed.get('tag', release_id)}")
            self._save()
            return {"deleted": release_id, "freed_bytes": freed}

    def logs(self, since=0, job_id=None, limit=200):
        """Activity records newer than `since` (a sequence number), oldest first, for following a job."""
        with self._lock:
            rows = [row for row in self._logs if row["sequence"] > since and (job_id is None or row.get("job_id") == job_id)]
            return {"logs": copy.deepcopy(rows[-max(1, min(int(limit), 500)):]),
                    "last_sequence": self._sequence}

    def register_manifest(self, manifest):
        manifest = validate_manifest(manifest)
        identity = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:16]
        with self._lock:
            release_id = "local-" + identity
            for env, item in manifest["firmware"].items():
                if item["sha256"] in self.transport._assets:
                    self._validate_image(env, item, self.transport._assets[item["sha256"]].read_bytes())
            if release_id not in self._releases:
                self._releases[release_id] = {"id": release_id, "source": "local", "tag": manifest["tag"],
                                              "created_at": time.time(), "built_at": manifest.get("built_at"),
                                              "manifest": manifest["firmware"]}
                self._save()
                self._log(f"Registered local manifest {manifest['tag']}")
            return self._release_view(self._releases[release_id])

    def upload_image(self, release_id, env, data):
        with self._lock:
            release = self._releases.get(release_id)
            if not release or release["source"] != "local" or env not in release["manifest"]:
                raise ValueError("unknown local release or firmware environment")
            image = release["manifest"][env]
            self._validate_image(env, image, data)
            path = self.cache / (image["sha256"] + ".bin")
            temporary = path.with_suffix(".tmp")
            temporary.write_bytes(data)
            temporary.replace(path)
            self.transport._assets[image["sha256"]] = path
            self._log(f"Uploaded {env}: {len(data):,} bytes")
            self._save()
            return self._release_view(release)

    @staticmethod
    def _validate_image(env, image, data):
        if len(data) != image["size"] or hashlib.sha256(data).hexdigest() != image["sha256"]:
            raise ValueError("image size or SHA-256 does not match manifest")
        if hashlib.md5(data).hexdigest() != image["md5"]:
            raise ValueError("image MD5 does not match manifest")
        chip = 9 if env in ("sensor", "screen_television") or env.startswith("arbiter") else 5 if env == "bridge" else 0
        if data[0] != 0xE9 or int.from_bytes(data[12:14], "little") != chip:
            raise ValueError("binary chip type does not match firmware environment")
        if image["build_hash"].encode() not in data:
            raise ValueError("binary does not contain the declared build identity")

    def _release_view(self, release):
        result = copy.deepcopy(release)
        for image in result["manifest"].values():
            image["uploaded"] = image["sha256"] in self.transport._assets
        result["ready"] = all(image["uploaded"] for image in result["manifest"].values())
        return result

    def snapshot(self):
        with self._lock:
            now = time.time()
            devices = []
            for node_id, fields in sorted(self.transport._inventory.items()):
                node = dict(fields, id=node_id, last_seen=self.transport._last_seen.get(node_id))
                node["online"] = bool(node["last_seen"] and now - node["last_seen"] < 90)
                node["release_ids"] = [release["id"] for release in self._releases.values()
                                       if release["manifest"].get(node.get("env"), {}).get("build_hash") == node.get("sha256")]
                devices.append(node)
            connected = getattr(self.transport.serial, "connected", None)
            if connected is None:
                connected = any(node["id"] == self.transport._bridge_id and node["online"] for node in devices)
            return copy.deepcopy({"now": now, "devices": devices,
                    "bridge": {"id": self.transport._bridge_id, "connected": connected,
                               "baud": getattr(self.transport.serial, "baudrate", None)},
                    "releases": [self._release_view(row) for row in sorted(self._releases.values(), key=lambda row: row["created_at"], reverse=True)],
                    "jobs": list(reversed(self._jobs)),
                    "active_job": self._active, "paused": self._paused, "queue": list(self._pending),
                    "logs": self._logs[-300:]})

    def _run_job(self, job):
        with self._lock:
            self._active = job["id"]
            job.update(state="running", started_at=time.time())
            self.transport.begin_job(job.get("exclude", []))
            self._save()
        try:
            self._log(f"Started {job['kind']}")
            if job["kind"] == "update":
                with self._lock:
                    release = copy.deepcopy(self._releases[job["release_id"]])
                results = self.transport.update_release(release, job["device_ids"], job["env"])
                if not job.get("cancel_requested"):
                    if not results:
                        raise RuntimeError("no devices answered with a matching environment")
                    if any(value["state"] == "failed" for value in results.values()):
                        raise RuntimeError("one or more devices failed; see device results")
            elif job["kind"] == "scan":
                nodes = self.transport.discover()
                if not job.get("cancel_requested"):
                    self._log(f"Census received {len(nodes)} device reports")
            with self._lock:
                if job.get("cancel_requested"):
                    job.update(state="cancelled", error="Cancelled")
                    self._log("Job cancelled")
                else:
                    job["state"] = "completed"
                    self._log("Job completed")
        except Exception as exc:
            with self._lock:
                if job.get("cancel_requested"):
                    job.update(state="cancelled", error="Cancelled")
                    self._log("Job cancelled")
                else:
                    job.update(state="failed", error=str(exc))
                    self._log(str(exc), level="error")
        finally:
            with self._lock:
                job["finished_at"] = time.time()
                self._active = None
                self.transport._transfer_target = None
                self._save()
            if job.get("cancel_requested"):
                # Give the bridge and device a moment to drop the abandoned download
                # before another job can offer them anything.
                self._stop.wait(3)

    def _worker(self):
        while not self._stop.is_set():
            job, signalled = None, True
            with self._wake:
                if self._paused or not self._pending:
                    signalled = self._wake.wait(30)
                if self._stop.is_set():
                    break
                if self._pending and not self._paused:
                    job_id = self._pending.pop(0)
                    job = next(row for row in self._jobs if row["id"] == job_id)
            if job is None:
                if not signalled:
                    # Idle for 30 s: passive discovery shares this worker, so it
                    # never issues control traffic in the middle of a transfer.
                    try:
                        self.transport.begin_job()
                        self.transport.discover(timeout=5)
                        with self._lock:
                            self._save()
                    except Exception:
                        pass
                continue
            self._run_job(job)
