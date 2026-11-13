"""Firmware update transport for the Overlord: census, and serving images to nodes over the bridge."""

import os
import threading
import time
from pathlib import Path

ACTIVE_ENVS = {"sensor", "screen", "component_state_display", "arbiter_mesh", "bridge"}
OPTIONAL_ENVS = {"screen_extractor", "screen_diorama", "screen_party", "screen_television", "bridge_esp32", "arbiter_crowpanel7"}
CACHE = Path(os.environ.get("OVERLORD_FIRMWARE_CACHE",
                            str(Path.home() / ".cache" / "overlord" / "firmware")))


class FirmwareUpdater:
    """Coordinate firmware updates over the bridge. Images are pushed to the
    Overlord (see FirmwareManager); nothing is downloaded from the internet.

    The serial protocol is line-oriented control only. Firmware bytes are served
    on demand in bounded Base64 chunks using FW-READ / FW-DATA records.
    """

    def __init__(self, serial_reader, observer=None, cache=None):
        self.serial = serial_reader
        self.observer = observer
        self.cache = Path(cache) if cache is not None else CACHE
        self._last_seen = {}
        self._transfer_target = None
        self._lock = threading.RLock()
        self._inventory = {}
        self._statuses = {}
        self._condition = threading.Condition(self._lock)
        self._assets = {}  # sha256 -> local Path
        self._release = None
        # What the queue can ask of an update that is running (FirmwareManager):
        # hold before the next device, stop, or skip a device. Guarded by _condition.
        self._paused = False
        self._cancel = False
        self._exclude = set()
        # The bridge on this machine's UART, from its FW-BRIDGE line. Satellite
        # players have bridges too, and census reports them with the same env;
        # only this one is updated over the UART, the rest over the mesh.
        self._bridge_id = None

    def handle_line(self, line):
        if line.startswith("PULLEYS-CENSUS "):
            fields = _fields(line)
            node_id = fields.get("id")
            if node_id:
                with self._condition:
                    key = node_id.upper()
                    self._inventory[key] = {**self._inventory.get(key, {}), **fields}
                    self._last_seen[key] = time.time()
                    self._emit("inventory", node_id=key)
                    self._condition.notify_all()
            return True
        if line.startswith("FW-NODE ") or line.startswith("FW-BRIDGE "):
            fields = _fields(line)
            node_id = fields.get("id")
            if node_id:
                with self._condition:
                    if line.startswith("FW-BRIDGE "):
                        self._bridge_id = node_id.upper()
                    key = node_id.upper()
                    self._inventory[key] = {**self._inventory.get(key, {}), **fields}
                    self._last_seen[key] = time.time()
                    self._emit("inventory", node_id=key)
                    self._condition.notify_all()
            return True
        if line.startswith("FW-STATUS "):
            fields = _fields(line)
            node_id = fields.get("id", "").upper()
            with self._condition:
                self._statuses[node_id] = fields
                self._emit("status", node_id=node_id, **{k: v for k, v in fields.items() if k != "node_id"})
                self._condition.notify_all()
            return True
        if line.startswith("FW-READ "):
            self._answer_chunk(line)
            return True
        return False

    def _emit(self, kind, **fields):
        if self.observer:
            self.observer(kind, fields)

    # ── what the queue can ask of a running update ───────────────────────────
    def begin_job(self, exclude=()):
        """A new job: nothing is cancelled, and these devices are skipped."""
        with self._condition:
            self._cancel = False
            self._exclude = set(exclude)

    def request_cancel(self):
        """Stop serving the device being flashed and take no more. A device that
        has not finished downloading discards the partial image, so this is safe
        until its install is acknowledged."""
        with self._condition:
            self._cancel = True
            self._condition.notify_all()

    def set_paused(self, paused):
        """Hold before the next device. The one being flashed finishes first."""
        with self._condition:
            self._paused = bool(paused)
            self._condition.notify_all()

    def exclude_device(self, node_id):
        with self._condition:
            self._exclude.add(node_id)

    @property
    def cancelled(self):
        return self._cancel

    def _hold_while_paused(self, node_id):
        with self._condition:
            announced = False
            while self._paused and not self._cancel:
                if not announced:
                    announced = True
                    self._emit("device", node_id=node_id, state="queued", message="queue paused; waiting to resume")
                self._condition.wait(1)

    def _settle(self, seconds):
        """Sleep, but wake at once if the job is cancelled."""
        with self._condition:
            end = time.monotonic() + seconds
            while not self._cancel and time.monotonic() < end:
                self._condition.wait(max(0, end - time.monotonic()))

    def discover(self, timeout=25):
        started = time.time()
        with self._condition:
            self._bridge_id = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.serial.write_line("FW-PING")
            with self._condition:
                if self._bridge_id is not None:
                    break
                self._condition.wait(min(1, max(0, deadline - time.monotonic())))
        if self._bridge_id is None:
            raise RuntimeError("bridge unavailable")
        # Merge two rounds: one radio census regularly misses a few replies.
        for _ in range(2):
            if self._cancel:
                break
            self.serial.write_line("FW-CENSUS spread=3000")
            self._settle(3.5)
        with self._condition:
            return {key: dict(value) for key, value in self._inventory.items()
                    if self._last_seen.get(key, 0) >= started}

    def update_release(self, release, device_ids=None, env=None):
        fresh = self.discover()
        table = release["manifest"]
        requested = set(device_ids) if device_ids is not None else None
        nodes = [(key, value) for key, value in fresh.items()
                 if (requested is None and value.get("env") in table or requested is not None and key in requested) and
                    (env is None or value.get("env") == env)]
        results = {}
        for missing in (requested or set()) - fresh.keys():
            results[missing] = {"state": "failed", "error": "device did not answer census"}
            self._emit("device", node_id=missing, **results[missing])
        table = release["manifest"]
        print(f"firmware: release {release['tag']} inventory: {len(nodes)} nodes", flush=True)
        for node_id, node in nodes:
            print(f"firmware: node {node_id} env={node.get('env', '?')} "
                  f"build_hash={node.get('sha256', 'unknown')}", flush=True)
        # Preserve the requested first/last priorities; all middle entries are
        # intentionally unordered.
        own = self._bridge_id
        nodes.sort(key=lambda item: 0 if str(item[1].get("env", "")).startswith("arbiter") else
                          2 if item[0] == own else 1)
        for node_id, node in nodes:
            self._hold_while_paused(node_id)
            if self._cancel:
                results[node_id] = {"state": "cancelled", "message": "cancelled before it started"}
                self._emit("device", node_id=node_id, **results[node_id])
                continue
            if node_id in self._exclude:
                results[node_id] = {"state": "skipped", "message": "removed from this job"}
                self._emit("device", node_id=node_id, **results[node_id])
                continue
            env = node.get("env")
            if env not in table or table[env]["sha256"] not in self._assets:
                results[node_id] = {"state": "failed", "error": "no uploaded image for this environment"}
                self._emit("device", node_id=node_id, **results[node_id])
                continue
            expected = table[env]["sha256"]
            expected_build = table[env]["build_hash"]
            if node.get("sha256") == expected_build:
                results[node_id] = {"state": "skipped", "message": "already current"}
                self._emit("device", node_id=node_id, **results[node_id])
                continue
            self._transfer_target = node_id
            self._emit("device", node_id=node_id, state="queued", total=table[env]["size"])
            is_own_bridge = node_id == own
            if is_own_bridge:
                command = (f"FW-BRIDGE-UPDATE env={env} build_hash={expected_build} "
                           f"sha256={expected} size={table[env]['size']}")
            else:
                command = (f"FW-OFFER id={node_id} env={env} build_hash={expected_build} "
                           f"sha256={expected} md5={table[env]['md5']} size={table[env]['size']}")
            for attempt in range(1, 4):
                if self._cancel:
                    results[node_id] = {"state": "cancelled", "message": "cancelled before it started"}
                    self._emit("device", node_id=node_id, **results[node_id])
                    break
                with self._condition:
                    self._statuses.pop(node_id, None)
                self._emit("device", node_id=node_id, state="offering", attempt=attempt,
                           bytes=0, total=table[env]["size"])
                if not self.serial.write_line(command):
                    results[node_id] = {"state": "failed", "error": "bridge disconnected"}
                    self._emit("device", node_id=node_id, **results[node_id])
                    break
                print(f"firmware: {env} {node_id} attempt {attempt}/3", flush=True)
                # Keep a conservative timeout for legacy 115200-baud bridges
                # while newer bridges use 921600 baud and 4 KiB chunks.
                timeout = max(180, table[env]["size"] / 4000 + 60)
                status = self._wait_status(node_id, timeout)
                if status and status.get("state") == "cancelled":
                    results[node_id] = {"state": "cancelled", "message": "cancelled while downloading; the device keeps its current firmware"}
                    self._emit("device", node_id=node_id, **results[node_id])
                    break
                if status and status.get("state") == "ok":
                    self._emit("device", node_id=node_id, state="verifying")
                    # Require a newly requested census report with the expected
                    # hash rather than accepting the install ACK alone.
                    self._settle(8)  # allow the target to restart and rejoin ESP-NOW
                    confirmed_after = time.time()
                    if is_own_bridge:
                        refreshed = None
                        reconnect_deadline = time.monotonic() + 90
                        while time.monotonic() < reconnect_deadline and not self._cancel:
                            self.serial.write_line("FW-PING")
                            with self._condition:
                                value = self._inventory.get(node_id)
                                if (value and value.get("sha256") == expected_build and
                                        self._last_seen.get(node_id, 0) >= confirmed_after):
                                    refreshed = value
                                    break
                                self._condition.wait(1)
                    else:
                        refreshed = None
                        for _ in range(18):
                            if self._cancel:
                                break
                            self.serial.write_line(f"FW-QUERY id={node_id}")
                            refreshed = self._wait_inventory(node_id, 5, expected_build, confirmed_after)
                            if refreshed:
                                break
                    if not refreshed and self._cancel:
                        # The install was already acknowledged, so it may well have gone through.
                        results[node_id] = {"state": "cancelled", "message":
                                            "cancelled while verifying; the device may have installed the new firmware"}
                        self._emit("device", node_id=node_id, **results[node_id])
                        break
                    if refreshed:
                        print(f"firmware: {env} {node_id} confirmed", flush=True)
                        results[node_id] = {"state": "confirmed"}
                        self._emit("device", node_id=node_id, state="confirmed")
                        break
                    print(f"firmware: {node_id} hash not confirmed after reboot", flush=True)
                else:
                    print(f"firmware: {node_id} update attempt failed", flush=True)
                results[node_id] = {"state": "failed", "error":
                                    "reboot identity not confirmed" if status and status.get("state") == "ok"
                                    else f"update failed ({status.get('err', 'unknown')})" if status
                                    else "update timed out"}
                self._emit("device", node_id=node_id, **results[node_id])
                if attempt < 3:
                    self._settle(2)
        self._transfer_target = None
        return results

    def _wait_status(self, node_id, seconds):
        end = time.monotonic() + seconds
        with self._condition:
            while time.monotonic() < end:
                value = self._statuses.get(node_id)
                if value and value.get("state") in ("ok", "failed"):
                    return value
                if self._cancel:
                    return {"state": "cancelled"}
                self._condition.wait(max(0, end - time.monotonic()))
        return None

    def _wait_inventory(self, node_id, seconds, expected=None, after=0):
        end = time.monotonic() + seconds
        with self._condition:
            while time.monotonic() < end:
                value = self._inventory.get(node_id)
                if (value and (expected is None or value.get("sha256") == expected) and
                        self._last_seen.get(node_id, 0) >= after):
                    return value
                self._condition.wait(max(0, end - time.monotonic()))
        return None

    def _answer_chunk(self, line):
        import base64
        import zlib
        fields = _fields(line)
        digest = fields.get("sha256", "").lower()
        if self._cancel:
            # Answer with an error so the bridge ends the download promptly; the
            # device then drops the partial image.
            self.serial.write_line(f"FW-ERROR sha256={digest} offset={fields.get('offset', '?')} reason=cancelled")
            return
        try:
            offset = int(fields["offset"])
            length = int(fields.get("length", 384))
            path = self._assets[digest]
            if offset < 0 or not 1 <= length <= 4096 or offset >= path.stat().st_size:
                raise ValueError("invalid chunk range")
            with path.open("rb") as stream:
                stream.seek(offset)
                chunk = stream.read(length)
            encoded = base64.b64encode(chunk).decode("ascii")
            if self.serial.write_line(f"FW-DATA sha256={digest} offset={offset} crc32={zlib.crc32(chunk):08x} b64={encoded}"):
                self._emit("progress", node_id=self._transfer_target, bytes=offset + len(chunk), sha256=digest)
        except Exception as exc:
            self.serial.write_line(f"FW-ERROR sha256={digest} offset={fields.get('offset','?')} reason=unavailable")
            print(f"firmware: cannot serve requested chunk: {exc}", flush=True)


def _fields(line):
    fields = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value.strip('"')
    return fields
