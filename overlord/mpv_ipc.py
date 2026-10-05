"""Spawn mpv and drive it over its JSON IPC socket."""

import itertools
import json
import os
import queue
import socket
import subprocess
import tempfile
import threading
import time

# Kiosk defaults: borderless fullscreen, no on-screen UI, never exit on its own.
KIOSK_ARGS = [
    "--fullscreen",
    "--no-border",
    "--idle=yes",
    "--keep-open=no",
    "--force-window=yes",
    "--osc=no",
    "--osd-level=0",
    "--cursor-autohide=always",
    "--hwdec=auto-safe",
    "--background-color=#000000",
]


# Audio-only player: no window, no terminal UI, stays alive between files.
AUDIO_ARGS = [
    "--no-video",
    "--no-terminal",
    "--idle=yes",
    "--keep-open=no",
]


class MpvError(Exception):
    pass


class Mpv:
    def __init__(self, extra_args=(), on_event=None, kiosk=True, name="video"):
        """kiosk=True: fullscreen video player. kiosk=False: headless audio player.
        name keeps the IPC socket unique when several instances share a process."""
        self.on_event = on_event
        self._sock_path = os.path.join(
            tempfile.gettempdir(), f"overlord-mpv-{os.getpid()}-{name}.sock"
        )
        self._ids = itertools.count(1)
        self._pending = {}  # request_id -> [threading.Event, response]
        self._lock = threading.Lock()

        self.proc = subprocess.Popen(
            [
                "mpv",
                f"--input-ipc-server={self._sock_path}",
                *(KIOSK_ARGS if kiosk else AUDIO_ARGS),
                *extra_args,
            ]
        )
        self._sock = self._connect()
        # Events are handled off the reader thread: handlers issue commands, and
        # command() waits on replies that only the reader thread can deliver.
        self._events = queue.Queue()
        threading.Thread(target=self._read_loop, daemon=True).start()
        threading.Thread(target=self._event_loop, daemon=True).start()

    def _connect(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise MpvError(f"mpv exited during startup (code {self.proc.returncode})")
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self._sock_path)
                return s
            except (FileNotFoundError, ConnectionRefusedError):
                s.close()
                time.sleep(0.05)
        raise MpvError("timed out waiting for mpv IPC socket")

    def _read_loop(self):
        for line in self._sock.makefile("rb"):
            msg = json.loads(line)
            if "request_id" in msg:
                with self._lock:
                    slot = self._pending.pop(msg["request_id"], None)
                if slot:
                    slot[1] = msg
                    slot[0].set()
            elif "event" in msg and self.on_event:
                self._events.put(msg)
        # Socket closed: mpv is gone. Fail anything still waiting.
        with self._lock:
            for slot in self._pending.values():
                slot[0].set()
            self._pending.clear()

    def _event_loop(self):
        while True:
            msg = self._events.get()
            try:
                self.on_event(msg)
            except Exception as e:
                print(f"event handler error: {e}", flush=True)

    def command(self, *args, timeout=5.0):
        rid = next(self._ids)
        slot = [threading.Event(), None]
        with self._lock:
            self._pending[rid] = slot
        payload = json.dumps({"command": list(args), "request_id": rid}) + "\n"
        try:
            self._sock.sendall(payload.encode())
        except OSError as e:
            raise MpvError(f"mpv connection lost: {e}") from e
        if not slot[0].wait(timeout):
            raise MpvError(f"timeout: {args}")
        resp = slot[1]
        if resp is None:
            raise MpvError(f"mpv connection lost: {args}")
        if resp.get("error") != "success":
            raise MpvError(f"{args}: {resp.get('error')}")
        return resp.get("data")

    def get(self, prop):
        return self.command("get_property", prop)

    def set(self, prop, value):
        return self.command("set_property", prop, value)

    def play(self, path, loop=True):
        self.set("loop-file", "inf" if loop else "no")
        self.command("loadfile", str(path), "replace")

    def stop(self):
        """Stop playback, leaving the player idle."""
        self.set("loop-file", "no")
        self.command("stop")

    def quit(self):
        if self.proc.poll() is None:
            try:
                self.command("quit", timeout=2.0)
            except MpvError:
                pass
            try:
                self.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self._sock.close()
        try:
            os.unlink(self._sock_path)
        except FileNotFoundError:
            pass


class SoundPlayer:
    """Fire-and-forget one-shot sounds. Each sound gets its own mpv process so
    they overlap freely with each other and with the audio loop."""

    def __init__(self, extra_args=()):
        self._extra = list(extra_args)
        self._procs = []
        self._lock = threading.Lock()

    def play(self, path):
        proc = subprocess.Popen(
            ["mpv", *AUDIO_ARGS, "--idle=no", *self._extra, str(path)],
            stdin=subprocess.DEVNULL,
        )
        with self._lock:
            self._procs = [p for p in self._procs if p.poll() is None]  # reap finished
            self._procs.append(proc)

    def stop_all(self):
        with self._lock:
            for p in self._procs:
                if p.poll() is None:
                    p.terminate()
            self._procs = []
