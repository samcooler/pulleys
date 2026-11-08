"""Authoritative arbiter configuration through the bridge's bounded IC protocol."""
import copy
from datetime import datetime
import json
import re
import threading
import time
import uuid

HEX_ID = re.compile(r"[0-9A-F]{4}")
TOKEN = re.compile(r"[0-9A-F]{16}")
FRESH_SECONDS = 15
MAX_CHUNKS = 64
CHUNK_BYTES = 200


class InstallationError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class InstallationManager:
    def __init__(self, serial, busy=lambda: False, timeout=18, retry=7):
        self.serial = serial
        self.busy = busy
        self.timeout = timeout
        self.retry = retry
        self._condition = threading.Condition()
        self._request_lock = threading.Lock()
        self._pending = None
        self._arbiters = {}
        self._error = None
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._poll, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        with self._condition:
            self._condition.notify_all()
        if self._thread:
            self._thread.join(timeout=2)

    def _poll(self):
        while not self._stop.is_set():
            try:
                self.request("get", wait_lock=False)
                self._error = None
            except InstallationError as exc:
                self._error = str(exc)
            self._stop.wait(3)

    def snapshot(self):
        with self._condition:
            now = time.monotonic()
            arbiters = []
            for row, seen in self._arbiters.values():
                view = copy.deepcopy(row)
                view["age_seconds"] = round(now - seen, 1)
                view["fresh"] = now - seen < FRESH_SECONDS and bool(self.serial.connected)
                arbiters.append(view)
            return {"bridge_connected": bool(self.serial.connected), "busy": self.busy(),
                    "arbiters": sorted(arbiters, key=lambda row: row["id"]), "error": self._error,
                    "timezone": datetime.now().astimezone().strftime("%Z (UTC%z)"),
                    "local_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S")}

    @staticmethod
    def validate(op, body):
        fields = {"color": {"channel": (1, 24), "color": (0, 5)},
                  "rgb": {"color": (0, 5), "r": (0, 255), "g": (0, 255), "b": (0, 255)},
                  "shuffle": {}, "reset_rgb": {}, "clock": {}, "clock_now": {},
                  "map_begin": {}, "map_again": {}, "map_renew": {}, "map_end": {},
                  "map_assign": {"channel": (1, 24)}}
        if op not in fields:
            raise ValueError("unsupported installation operation")
        required = {"target", "boot", "revision"} | fields[op].keys()
        if op.startswith("map_"):
            required.add("session")
            if not isinstance(body.get("session"), str) or not TOKEN.fullmatch(body["session"]) or int(body["session"], 16) == 0:
                raise ValueError("invalid mapping session")
        if op == "clock":
            required.add("time")
            text = body.get("time")
            if not isinstance(text, str) or not re.fullmatch(r"[0-9]{14}", text):
                raise ValueError("time must use YYYYMMDDHHMMSS in installation local time")
            try:
                date = datetime.strptime(text, "%Y%m%d%H%M%S")
                if not 2024 <= date.year <= 2099:
                    raise ValueError("year outside supported range")
            except ValueError:
                raise ValueError("invalid calendar date or time (years 2024–2099)")
        if set(body) != required:
            raise ValueError("invalid or missing operation fields")
        if not isinstance(body["target"], str) or not HEX_ID.fullmatch(body["target"]) or body["target"] == "0000":
            raise ValueError("target must be a nonzero four-digit uppercase arbiter ID")
        if not isinstance(body["boot"], str) or not TOKEN.fullmatch(body["boot"]):
            raise ValueError("invalid arbiter boot identity")
        if type(body["revision"]) is not int or not 0 <= body["revision"] <= 0xffffffff:
            raise ValueError("invalid configuration revision")
        for key, (lo, hi) in fields[op].items():
            if type(body[key]) is not int or not lo <= body[key] <= hi:
                raise ValueError(f"{key} must be an integer from {lo} to {hi}")

    def mutate(self, op, body):
        self.validate(op, body)
        with self._condition:
            cached = self._arbiters.get(body["target"])
            if not cached or time.monotonic() - cached[1] >= FRESH_SECONDS:
                raise InstallationError(503, "arbiter snapshot unavailable or stale")
        if op == "clock_now":
            body = {**body, "time": datetime.now().strftime("%Y%m%d%H%M%S")}
            op = "clock"
        return self.request(op, body)

    def request(self, op, body=None, wait_lock=True):
        # HTTP callers fail promptly instead of accumulating unbounded work.
        if not self._request_lock.acquire(timeout=3 if wait_lock else 0):
            raise InstallationError(409, "installation request in progress")
        try:
            if not self.serial.connected:
                raise InstallationError(503, "bridge disconnected")
            if self.busy():
                raise InstallationError(409, "firmware work in progress")
            q = uuid.uuid4().hex[:16].upper()
            request = {"v": 1, "q": q, "target": "0000", "op": op, **(body or {})}
            wire = json.dumps(request, separators=(",", ":"), allow_nan=False)
            if len(wire.encode()) > 241:
                raise ValueError("installation request exceeds mesh payload")
            pending = {"q": q, "target": request["target"], "chunks": {}, "responses": {}}
            with self._condition:
                self._pending = pending
            deadline = time.monotonic() + self.timeout
            next_send = 0
            discovery_done = None
            while not self._stop.is_set():
                now = time.monotonic()
                if self.busy() or not self.serial.connected:
                    raise InstallationError(503, "transport became unavailable; result may be unknown")
                if now >= next_send:
                    if not self.serial.write_line("INSTALL " + wire):
                        raise InstallationError(503, "bridge write failed; result may be unknown")
                    next_send = now + self.retry
                with self._condition:
                    if pending["responses"]:
                        if request["target"] != "0000":
                            response = pending["responses"][request["target"]]
                            break
                        # Leave time for another arbiter's reply. All known
                        # arbiters remain visible and mutations target one ID.
                        if discovery_done is None:
                            discovery_done = now + 1
                        complete = all(len(chunks["parts"]) == chunks["count"] for chunks in pending["chunks"].values())
                        if now >= discovery_done and complete:
                            response = next(iter(pending["responses"].values()))
                            break
                    if now >= deadline:
                        raise InstallationError(504, "arbiter response timed out; result may be unknown")
                    self._condition.wait(min(.2, max(0, deadline - now)))
            else:
                raise InstallationError(503, "installation manager stopped")
            if not response.get("ok"):
                error = response.get("error", "arbiter rejected operation")
                raise InstallationError(409 if error in {"conflict", "mapping_busy", "mapping_session_lost"} else 400, error)
            return response
        finally:
            with self._condition:
                self._pending = None
            self._request_lock.release()

    def handle_line(self, line):
        if not line.startswith("IC "):
            return False
        # IC <arbiter> <request> <bridge> <part> <count> <hex fragment>
        parts = line.split(" ", 6)
        if len(parts) != 7:
            return True
        _, arbiter, q, bridge, part_text, count_text, fragment = parts
        if not HEX_ID.fullmatch(arbiter) or not HEX_ID.fullmatch(bridge) or not TOKEN.fullmatch(q):
            return True
        try:
            fragment = bytes.fromhex(fragment).decode("ascii")
            part, count = int(part_text), int(count_text)
            if not 1 <= count <= MAX_CHUNKS or not 0 <= part < count or not 1 <= len(fragment.encode()) <= CHUNK_BYTES:
                return True
        except (ValueError, UnicodeError):
            return True
        with self._condition:
            pending = self._pending
            if not pending or pending["q"] != q or pending["target"] not in ("0000", arbiter):
                return True
            key = (arbiter, bridge)
            if key not in pending["chunks"] and len(pending["chunks"]) >= 8:
                return True
            chunks = pending["chunks"].setdefault(key, {"count": count, "parts": {}})
            if chunks["count"] != count or (part in chunks["parts"] and chunks["parts"][part] != fragment):
                return True
            chunks["parts"][part] = fragment
            if len(chunks["parts"]) != count:
                return True
            try:
                response = json.loads("".join(chunks["parts"][i] for i in range(count)))
                if (not isinstance(response, dict) or type(response.get("v")) is not int or
                    response.get("v") != 1 or type(response.get("ok")) is not bool):
                    return True
                if not response["ok"] and not isinstance(response.get("error"), str):
                    return True
                row = response.get("installation")
                if row is not None:
                    if (not isinstance(row, dict) or row.get("id") != arbiter or
                        not isinstance(row.get("boot"), str) or not TOKEN.fullmatch(row["boot"]) or
                        type(row.get("revision")) is not int):
                        return True
                    self._arbiters[arbiter] = (row, time.monotonic())
                elif response["ok"]:
                    return True
                pending["responses"][arbiter] = response
                self._condition.notify_all()
            except (ValueError, KeyError, TypeError):
                pass
        return True
