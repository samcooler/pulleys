"""Keyboard input for the debug menu.

The video mpv draws straight to DRM/KMS on the Pi, so it has no window to
receive key presses. EvdevKeys reads the keyboards in /dev/input directly
(the service user is in the `input` group). In a dev window, mpv_bindings()
routes the same keys through mpv instead.

Both produce the same key names: ESC UP DOWN LEFT RIGHT ENTER BS TAB, or the
single character typed.
"""

import os
import selectors
import struct
import threading
import time

EVENT = struct.Struct("llHHi")  # struct input_event on 64-bit Linux: time, type, code, value
EV_KEY = 1
KEY_LSHIFT, KEY_RSHIFT = 42, 54
RESCAN = 3.0  # seconds between looks for newly plugged keyboards

NAMED = {1: "ESC", 14: "BS", 15: "TAB", 28: "ENTER", 96: "ENTER",
         103: "UP", 105: "LEFT", 106: "RIGHT", 108: "DOWN"}


def _keymap():
    """evdev key code -> (plain, shifted), US layout."""
    m = {}
    for codes, plain, shifted in [
        (range(2, 14), "1234567890-=", "!@#$%^&*()_+"),
        (range(16, 28), "qwertyuiop[]", "QWERTYUIOP{}"),
        (range(30, 42), "asdfghjkl;'`", 'ASDFGHJKL:"~'),
        (range(43, 54), "\\zxcvbnm,./", "|ZXCVBNM<>?"),
    ]:
        for code, p, s in zip(codes, plain, shifted):
            m[code] = (p, s)
    m[57] = (" ", " ")
    return m


CHARS = _keymap()


def mpv_bindings():
    """(mpv key name, argument for the dbg-key script-message) for every key the
    menu uses. Characters travel as "c:<codepoint>" so mpv's command parser
    never sees quotes, ';' or '$'; decode with from_mpv()."""
    pairs = [(k, k) for k in ("ESC", "UP", "DOWN", "LEFT", "RIGHT", "ENTER", "BS", "TAB")]
    pairs += [("KP_ENTER", "ENTER"), ("SPACE", "c:32"), ("SHARP", "c:35")]
    for plain, shifted in CHARS.values():
        for ch in {plain, shifted} - {" ", "#"}:
            pairs.append((ch, f"c:{ord(ch)}"))
    return pairs


def from_mpv(arg):
    """Inverse of the argument half of mpv_bindings()."""
    return chr(int(arg[2:])) if arg.startswith("c:") else arg


class EvdevKeys:
    def __init__(self, on_key):
        self.on_key = on_key
        self._sel = selectors.DefaultSelector()
        self._files = {}  # path -> open file
        self._shift = False
        self._running = False

    def start(self):
        self._running = True
        threading.Thread(target=self._loop, daemon=True).start()

    def stop(self):
        self._running = False

    def _scan(self):
        try:
            names = os.listdir("/dev/input")
        except OSError:
            return
        for name in names:
            path = f"/dev/input/{name}"
            if not name.startswith("event") or path in self._files:
                continue
            try:
                f = open(path, "rb", buffering=0)
                os.set_blocking(f.fileno(), False)
            except OSError:
                continue  # no permission or gone again
            self._files[path] = f
            self._sel.register(f, selectors.EVENT_READ, path)

    def _drop(self, path):
        f = self._files.pop(path, None)
        if f:
            self._sel.unregister(f)
            f.close()

    def _loop(self):
        next_scan = 0.0
        while self._running:
            if time.monotonic() >= next_scan:
                self._scan()
                next_scan = time.monotonic() + RESCAN
            if not self._files:
                time.sleep(0.5)
                continue
            for key, _ in self._sel.select(timeout=0.5):
                try:
                    data = key.fileobj.read(EVENT.size * 64)
                except OSError:
                    self._drop(key.data)  # unplugged
                    continue
                if not data:
                    continue
                for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    self._handle(*EVENT.unpack_from(data, off)[2:])

    def _handle(self, etype, code, value):
        if etype != EV_KEY:
            return
        if code in (KEY_LSHIFT, KEY_RSHIFT):
            self._shift = value != 0
            return
        if value == 0:  # release; 1 is press, 2 is autorepeat
            return
        name = NAMED.get(code)
        if name is None and code in CHARS:
            name = CHARS[code][self._shift]
        if name:
            self.on_key(name)
