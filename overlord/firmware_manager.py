"""Durable firmware catalog and one queue shared by startup, HTTP and scripts."""
import copy
import hashlib
import json
from pathlib import Path
import queue
import re
import threading
import time
import uuid

from firmware import ACTIVE_ENVS, OPTIONAL_ENVS, CACHE, FirmwareUpdater

ENVS = ACTIVE_ENVS | OPTIONAL_ENVS
MAX_IMAGE = 7 * 1024 * 1024
TERMINAL = {"completed", "failed", "interrupted"}


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or type(manifest.get("schema")) is not int or manifest.get("schema") != 1:
        raise ValueError("manifest must use release schema 1")
    if not isinstance(manifest.get("tag"), str) or not 1 <= len(manifest["tag"]) <= 128:
        raise ValueError("manifest needs a tag")
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
        self._queue = queue.Queue()
        self._stop = threading.Event()
        self._thread = None
        self._releases = {}
        self._latest = None
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
                validate_manifest({"schema": 1, "tag": release["tag"], "firmware": release["manifest"]})
                self._releases[release["id"]] = release
            self._latest = state.get("latest")
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
        state = {"releases": list(self._releases.values()), "latest": self._latest,
                 "jobs": self._jobs[-100:], "logs": self._logs[-500:],
                 "inventory": self.transport._inventory, "last_seen": self.transport._last_seen,
                 "assets": {key: str(path.relative_to(self.cache)) for key, path in self.transport._assets.items()}}
        temporary = self.cache / "dashboard.json.tmp"
        temporary.write_text(json.dumps(state))
        temporary.replace(self.cache / "dashboard.json")

    def _log(self, message, node_id=None, level="info"):
        with self._lock:
            self._sequence += 1
            self._logs.append({"sequence": self._sequence, "at": time.time(), "message": message,
                               "node_id": node_id, "job_id": self._active, "level": level})
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

    def start(self, automatic=True):
        with self._lock:
            if self._thread:
                return
            self._thread = threading.Thread(target=self._worker, name="firmware-queue", daemon=True)
            if automatic:
                self.enqueue("startup")
            self._thread.start()

    def stop(self):
        self._stop.set()
        self._queue.put(None)
        if self._thread:
            self._thread.join(timeout=2)
        with self._lock:
            self._save()

    def enqueue(self, kind, release_id=None, device_ids=None, env=None):
        if kind not in {"startup", "check", "scan", "update"}:
            raise ValueError("unsupported job type")
        with self._lock:
            if sum(row["state"] in {"queued", "running"} for row in self._jobs) >= 25:
                raise ValueError("firmware queue is full")
            if kind == "update":
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
            job = {"id": uuid.uuid4().hex[:12], "kind": kind, "state": "queued", "created_at": time.time(),
                   "release_id": release_id, "device_ids": device_ids, "env": env, "devices": {}}
            self._jobs.append(job)
            # Never discard a queued/running job when keeping bounded history.
            completed = [row for row in self._jobs if row["state"] in TERMINAL]
            for row in completed[:-75]:
                self._jobs.remove(row)
            self._save()
            self._queue.put(job["id"])
            return copy.deepcopy(job)

    def retry(self, job_id):
        with self._lock:
            job = next((row for row in self._jobs if row["id"] == job_id), None)
            if not job or job["state"] not in {"failed", "interrupted"}:
                raise ValueError("only failed or interrupted jobs can be retried")
            if job["kind"] == "update" or job.get("release_id"):
                failed = [key for key, value in job["devices"].items() if value.get("state") == "failed"]
                return self.enqueue("update", job["release_id"], failed or job["device_ids"], job["env"])
            return self.enqueue(job["kind"])

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
                                              "created_at": time.time(), "manifest": manifest["firmware"]}
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
        chip = 9 if env == "sensor" or env.startswith("arbiter") else 5 if env == "bridge" else 0
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
                    "latest_release_id": self._latest, "jobs": list(reversed(self._jobs)),
                    "active_job": self._active, "logs": self._logs[-300:], "automatic_startup": True})

    def _check_release(self):
        self._log("Checking GitHub release and verifying assets")
        self.transport._fetch_latest()
        release = self.transport._release
        identity = hashlib.sha256(json.dumps(release["manifest"], sort_keys=True).encode()).hexdigest()[:12]
        release_id = "github-" + release["tag"] + "-" + identity
        with self._lock:
            self._releases[release_id] = {"id": release_id, "source": "github", "tag": release["tag"],
                                          "created_at": time.time(), "manifest": copy.deepcopy(release["manifest"])}
            self._latest = release_id
            self._save()
        self._log(f"Verified GitHub release {release['tag']}")
        return release_id

    def _run_job(self, job):
        with self._lock:
            self._active = job["id"]
            job.update(state="running", started_at=time.time())
            self._save()
        try:
            self._log(f"Started {job['kind']}")
            if job["kind"] in {"check", "startup"}:
                for attempt in range(3):
                    try:
                        job["release_id"] = self._check_release()
                        break
                    except Exception:
                        if attempt == 2:
                            raise
                        self._stop.wait((attempt + 1) * 3)
            if job["kind"] in {"update", "startup"}:
                with self._lock:
                    release = copy.deepcopy(self._releases[job["release_id"]])
                results = self.transport.update_release(release, job["device_ids"], job["env"])
                if not results:
                    raise RuntimeError("no devices answered with a matching environment")
                if any(value["state"] == "failed" for value in results.values()):
                    raise RuntimeError("one or more devices failed; see device results")
            elif job["kind"] == "scan":
                nodes = self.transport.discover()
                self._log(f"Census received {len(nodes)} device reports")
            with self._lock:
                job["state"] = "completed"
                self._log("Job completed")
        except Exception as exc:
            with self._lock:
                job.update(state="failed", error=str(exc))
                self._log(str(exc), level="error")
        finally:
            with self._lock:
                job["finished_at"] = time.time()
                self._active = None
                self.transport._transfer_target = None
                self._save()

    def _worker(self):
        while not self._stop.is_set():
            try:
                job_id = self._queue.get(timeout=30)
            except queue.Empty:
                # Passive discovery shares this worker: never issue control
                # traffic in the middle of a firmware transfer.
                try:
                    self.transport.discover(timeout=5)
                    with self._lock:
                        self._save()
                except Exception:
                    pass
                continue
            if job_id is None:
                break
            with self._lock:
                job = next(row for row in self._jobs if row["id"] == job_id)
            self._run_job(job)
            self._queue.task_done()
