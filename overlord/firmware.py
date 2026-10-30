"""Boot-time GitHub firmware update coordinator for the Overlord."""

import hashlib
import json
import os
import re
import subprocess
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ACTIVE_ENVS = {"sensor", "screen", "component_state_display", "arbiter_mesh", "bridge"}
CACHE = Path(os.environ.get("OVERLORD_FIRMWARE_CACHE",
                            str(Path.home() / ".cache" / "overlord" / "firmware")))


def _github_repo():
    explicit = os.environ.get("PULLEYS_GITHUB_REPO")
    if explicit:
        return explicit.strip().removesuffix(".git")
    try:
        url = subprocess.check_output(
            ["git", "remote", "get-url", "origin"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except Exception as exc:
        raise RuntimeError("cannot determine GitHub repository; set PULLEYS_GITHUB_REPO") from exc
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
    if not match:
        raise RuntimeError(f"origin is not a GitHub URL: {url}")
    return match.group(1)


def _get(url, timeout=15):
    headers = {"User-Agent": "pulleys-overlord", "Accept": "application/vnd.github+json"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


class FirmwareUpdater:
    """Fetch and validate the latest release, then coordinate serial updates.

    The serial protocol is line-oriented control only. Firmware bytes are served
    on demand in bounded Base64 chunks using FW-READ / FW-DATA records.
    """

    def __init__(self, serial_reader):
        self.serial = serial_reader
        self._lock = threading.RLock()
        self._inventory = {}
        self._statuses = {}
        self._condition = threading.Condition(self._lock)
        self._assets = {}  # sha256 -> local Path
        self._release = None
        self._running = False
        # The bridge on this machine's UART, from its FW-BRIDGE line. Satellite
        # players have bridges too, and census reports them with the same env;
        # only this one is updated over the UART, the rest over the mesh.
        self._bridge_id = None

    def start(self):
        if self._running:
            return
        self._running = True
        threading.Thread(target=self._boot_run, name="firmware-updater", daemon=True).start()

    def handle_line(self, line):
        if line.startswith("PULLEYS-CENSUS "):
            fields = _fields(line)
            node_id = fields.get("id")
            if node_id:
                with self._condition:
                    self._inventory[node_id.upper()] = fields
                    self._condition.notify_all()
            return True
        if line.startswith("FW-NODE ") or line.startswith("FW-BRIDGE "):
            fields = _fields(line)
            node_id = fields.get("id")
            if node_id:
                with self._condition:
                    if line.startswith("FW-BRIDGE "):
                        self._bridge_id = node_id.upper()
                    self._inventory[node_id.upper()] = fields
                    self._condition.notify_all()
            return True
        if line.startswith("FW-STATUS "):
            fields = _fields(line)
            node_id = fields.get("id", "").upper()
            with self._condition:
                self._statuses[node_id] = fields
                self._condition.notify_all()
            return True
        if line.startswith("FW-READ "):
            self._answer_chunk(line)
            return True
        return False

    def _fetch_latest(self):
        repo = _github_repo()
        release = json.loads(_get(f"https://api.github.com/repos/{repo}/releases/latest"))
        tag = release.get("tag_name")
        assets = {asset["name"]: asset["browser_download_url"] for asset in release.get("assets", [])}
        if "manifest.json" not in assets:
            raise RuntimeError(f"release {tag} has no manifest.json")
        manifest = json.loads(_get(assets["manifest.json"]))
        if manifest.get("tag") != tag or manifest.get("schema") != 1:
            raise RuntimeError("release manifest tag/schema does not match its release")
        table = manifest.get("firmware", {})
        if not ACTIVE_ENVS.issubset(table):
            raise RuntimeError("release manifest is missing active firmware environments")
        CACHE.mkdir(parents=True, exist_ok=True)
        local = {}
        for env in sorted(ACTIVE_ENVS):
            entry = table[env]
            name = entry.get("asset")
            if name not in assets:
                raise RuntimeError(f"release is missing asset {name!r} for {env}")
            digest = entry.get("sha256", "").lower()
            build_hash = entry.get("build_hash", "").lower()
            md5 = entry.get("md5", "").lower()
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise RuntimeError(f"invalid SHA-256 for {env}")
            if not re.fullmatch(r"[0-9a-f]{64}", build_hash):
                raise RuntimeError(f"invalid build hash for {env}")
            if not re.fullmatch(r"[0-9a-f]{32}", md5):
                raise RuntimeError(f"invalid migration checksum for {env}")
            data = _get(assets[name], timeout=60)
            if (len(data) != entry.get("size") or
                    hashlib.sha256(data).hexdigest() != digest):
                raise RuntimeError(f"download integrity check failed for {env}")
            if hashlib.md5(data, usedforsecurity=False).hexdigest() != md5:
                raise RuntimeError(f"download MD5 check failed for {env}")
            path = CACHE / f"{tag}-{env}-{digest}.bin"
            path.write_bytes(data)
            local[digest] = path
            table[env]["sha256"] = digest
        self._release = {"tag": tag, "manifest": table}
        self._assets = local

    def _boot_run(self):
        for attempt in range(1, 4):
            try:
                self._fetch_latest()
                break
            except Exception as exc:
                print(f"firmware: GitHub attempt {attempt}/3 failed: {exc}", flush=True)
                if attempt == 3:
                    return
                time.sleep(attempt * 3)

        # Let the normal Overlord stay online; this thread waits briefly for the
        # bridge and then performs one boot-scoped inventory/update pass.
        with self._condition:
            self._inventory.clear()
            self._bridge_id = None
        deadline = time.monotonic() + 25
        bridge_seen = False
        while time.monotonic() < deadline:
            # The first FW- line is also what makes our bridge raise the
            # firmware AP (src/bridge), so this has to come before any offer.
            self.serial.write_line("FW-PING")
            with self._condition:
                bridge_seen = self._bridge_id is not None
                if bridge_seen:
                    break
                self._condition.wait(1)
        if not bridge_seen:
            print("firmware: bridge unavailable; skipping until next boot", flush=True)
            return
        with self._condition:
            self._inventory.clear()
            self._statuses.clear()
        self.serial.write_line("FW-CENSUS spread=3000")
        time.sleep(4.5)
        if not self._inventory:
            print("firmware: no nodes found; skipping until next boot", flush=True)
            return

        table = self._release["manifest"]
        nodes = list(self._inventory.items())
        # Preserve the requested first/last priorities; all middle entries are
        # intentionally unordered.
        own = self._bridge_id
        nodes.sort(key=lambda item: 0 if item[1].get("env") == "arbiter_mesh" else
                          2 if item[0] == own else 1)
        for node_id, node in nodes:
            env = node.get("env")
            if env not in table:
                continue
            expected = table[env]["sha256"]
            expected_build = table[env]["build_hash"]
            if node.get("sha256") == expected_build:
                continue
            is_own_bridge = env == "bridge" and node_id == own
            if is_own_bridge:
                command = (f"FW-BRIDGE-UPDATE env={env} build_hash={expected_build} "
                           f"sha256={expected} size={table[env]['size']}")
            else:
                command = (f"FW-OFFER id={node_id} env={env} build_hash={expected_build} "
                           f"sha256={expected} md5={table[env]['md5']} size={table[env]['size']}")
            for attempt in range(1, 4):
                with self._condition:
                    self._statuses.pop(node_id, None)
                if not self.serial.write_line(command):
                    break
                print(f"firmware: {env} {node_id} attempt {attempt}/3", flush=True)
                status = self._wait_status(node_id, 180)
                if status and status.get("state") == "ok":
                    # Require a newly requested census report with the expected
                    # hash rather than accepting the install ACK alone.
                    time.sleep(8)  # allow the target to restart and rejoin ESP-NOW
                    with self._condition:
                        self._inventory.pop(node_id, None)
                    if is_own_bridge:
                        refreshed = None
                        reconnect_deadline = time.monotonic() + 90
                        while time.monotonic() < reconnect_deadline:
                            self.serial.write_line("FW-PING")
                            with self._condition:
                                value = self._inventory.get(node_id)
                                if value and value.get("sha256") == expected_build:
                                    refreshed = value
                                    break
                                self._condition.wait(1)
                    else:
                        self.serial.write_line(f"FW-QUERY id={node_id}")
                        refreshed = self._wait_inventory(node_id, 90, expected_build)
                    if refreshed:
                        print(f"firmware: {env} {node_id} confirmed", flush=True)
                        break
                    print(f"firmware: {node_id} hash not confirmed after reboot", flush=True)
                else:
                    print(f"firmware: {node_id} update attempt failed", flush=True)
                if attempt < 3:
                    time.sleep(2)

    def _wait_status(self, node_id, seconds):
        end = time.monotonic() + seconds
        with self._condition:
            while time.monotonic() < end:
                value = self._statuses.get(node_id)
                if value and value.get("state") in ("ok", "failed"):
                    return value
                self._condition.wait(max(0, end - time.monotonic()))
        return None

    def _wait_inventory(self, node_id, seconds, expected=None):
        end = time.monotonic() + seconds
        with self._condition:
            while time.monotonic() < end:
                value = self._inventory.get(node_id)
                if value and (expected is None or value.get("sha256") == expected):
                    return value
                self._condition.wait(max(0, end - time.monotonic()))
        return None

    def _answer_chunk(self, line):
        import base64
        fields = _fields(line)
        digest = fields.get("sha256", "").lower()
        try:
            offset = int(fields["offset"])
            length = min(384, int(fields.get("length", 384)))
            path = self._assets[digest]
            with path.open("rb") as stream:
                stream.seek(offset)
                chunk = stream.read(length)
            encoded = base64.b64encode(chunk).decode("ascii")
            self.serial.write_line(f"FW-DATA sha256={digest} offset={offset} b64={encoded}")
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
