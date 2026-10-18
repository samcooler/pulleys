"""Debug menu: press Escape on the panel's keyboard to open it.

Drawn as an ASS overlay on the video mpv, driven by whatever feeds on_key()
(keys.EvdevKeys on the panel, mpv key bindings in a dev window). Screens:

  menu -> Wi-Fi: scan -> pick a network -> type the password -> connect
       -> Network info, Restart overlord, Close
"""

import textwrap
import threading
import time

import wifi
from simulate import ass_rect, ass_text

OVERLAY_ID = 8  # above the simulator's bars (7)
RES_X, RES_Y = 1920, 1080
MARGIN = 96
ROW_H = 68
VISIBLE_ROWS = 10
MAX_TEXT = 63  # longest WPA passphrase
STATUS_EVERY = 5.0  # seconds between status refreshes while the menu is open

GOLD = "FFCC00"
GREY = "9A9A9A"
GREEN = "5CE16B"
RED = "FF6B60"


def escape_ass(text):
    """Make user-controlled text (SSIDs!) inert inside an ASS event."""
    return text.replace("\\", "\\⁠").replace("{", "\\{")


class DebugMenu:
    def __init__(self, video, restart):
        """video: the video Mpv, which hosts the overlay. restart: callable that
        ends the process so the service manager starts it again."""
        self.video = video
        self.restart = restart
        self._lock = threading.RLock()
        self.open = False
        self.status = None  # None until the first check finishes
        self._polls = 0  # which status poller is current, so a reopen retires the old one
        self._reset()

    def _reset(self):
        self.screen = "menu"
        self.sel = 0
        self.networks = []
        self.target = None  # network being joined: {"ssid", "secured"}
        self.hidden = False
        self.text = ""
        self.reveal = False
        self.title = ""
        self.lines = []  # message / info body

    # --- input -------------------------------------------------------------

    def on_key(self, key):
        """Esc opens the menu anywhere; everything else only counts while open."""
        with self._lock:
            if not self.open:
                if key == "ESC":
                    self.open = True
                    self.status = None
                    self._reset()
                    self._polls += 1
                    threading.Thread(target=self._poll_status, args=(self._polls,),
                                     daemon=True).start()
                    self._draw()
                return
            if key == "ESC":
                self._back()
            elif self.screen == "menu":
                self._list_key(key, self._menu_items(), self._menu_pick)
            elif self.screen == "networks":
                self._list_key(key, self._network_rows(), self._network_pick)
            elif self.screen in ("ssid", "password"):
                self._text_key(key)
            elif self.screen == "message" and key == "ENTER":
                self._back()
            self._draw()

    def _back(self):
        if self.screen in ("scanning", "working"):
            return  # a worker thread is mid-flight; it moves the screen on when done
        if self.screen == "menu":
            self.open = False
        elif self.screen in ("ssid", "password"):
            self.screen, self.sel = "networks", 0
        else:
            self._reset()

    def _list_key(self, key, rows, pick):
        if key == "UP":
            self.sel = (self.sel - 1) % len(rows)
        elif key == "DOWN":
            self.sel = (self.sel + 1) % len(rows)
        elif key == "ENTER":
            pick(self.sel)

    def _text_key(self, key):
        if key == "ENTER":
            if self.screen == "ssid":
                if self.text:
                    self.hidden = True
                    self.target = {"ssid": self.text, "secured": True}
                    self.text, self.reveal, self.screen = "", False, "password"
            else:
                self._join(self.target["ssid"], self.text, self.hidden)
        elif key == "BS":
            self.text = self.text[:-1]
        elif key == "TAB" and self.screen == "password":
            self.reveal = not self.reveal
        elif len(key) == 1 and len(self.text) < MAX_TEXT:
            self.text += key

    # --- menu --------------------------------------------------------------

    def _menu_items(self):
        return ["Reconfigure Wi-Fi", "Network info", "Restart overlord", "Close menu"]

    def _menu_pick(self, i):
        if i == 0:
            self._scan()
        elif i == 1:
            self._work("Network info", wifi.status)
        elif i == 2:
            self.screen, self.title = "working", "Restarting overlord..."
            self.restart()
        else:
            self.open = False

    # --- wifi --------------------------------------------------------------

    def _network_rows(self):
        return self.networks + [{"ssid": "Hidden network...", "other": True}]

    def _network_pick(self, i):
        net = self._network_rows()[i]
        self.text, self.reveal = "", False
        if net.get("other"):
            self.screen, self.hidden = "ssid", False
        elif net["secured"]:
            self.screen, self.hidden, self.target = "password", False, net
        else:
            self._join(net["ssid"], None, False)

    def _scan(self):
        self.screen, self.title = "scanning", "Scanning for networks..."
        self._run(wifi.scan, self._scanned)

    def _scanned(self, networks, err):
        if err:
            self._message("Wi-Fi scan failed", [err])
        else:
            self.screen, self.sel, self.networks = "networks", 0, networks

    def _join(self, ssid, password, hidden):
        self.screen, self.title = "working", f"Connecting to {ssid}..."

        def done(_, err):
            if err:
                self._message("Could not connect", [err])
            else:
                self._message("Connected", [f"Joined {ssid}."])
        self._run(lambda: wifi.connect(ssid, password, hidden), done)

    def _work(self, title, fn):
        self.screen, self.title = "working", title
        self._run(fn, lambda lines, err: self._message(title, [err] if err else lines))

    def _message(self, title, lines):
        wrapped = [w for l in lines for w in textwrap.wrap(l, 60, subsequent_indent="    ") or [""]]
        self.screen, self.title, self.lines = "message", title, wrapped

    def _run(self, fn, done):
        """Run fn off the input thread (nmcli takes seconds), then done(result, error)."""
        def worker():
            try:
                result, err = fn(), None
            except wifi.WifiError as e:
                result, err = None, str(e)
            except Exception as e:
                result, err = None, f"{type(e).__name__}: {e}"
            with self._lock:
                if self.open:
                    done(result, err)
                    self._draw()
        threading.Thread(target=worker, daemon=True).start()

    def _poll_status(self, gen):
        """While the menu is open, keep the header's Wi-Fi/IP/internet lines fresh."""
        while True:
            try:
                info = wifi.link()
            except wifi.WifiError as e:
                info = {"error": str(e), "wifi": None, "ip": None}
            info["internet"] = wifi.internet()
            with self._lock:
                if not self.open or gen != self._polls:
                    return
                self.status = info
                self._draw()
            for _ in range(int(STATUS_EVERY * 10)):
                time.sleep(0.1)
                if not self.open or gen != self._polls:
                    return

    def _status_lines(self):
        s = self.status
        if s is None:
            return [("Checking network...", GREY)]
        if "error" in s:
            wifi_line = ("Wi-Fi: unavailable (" + s["error"] + ")", RED)
        elif s["wifi"]:
            wifi_line = (f"Wi-Fi: connected to {s['wifi']}", GREEN)
        else:
            wifi_line = ("Wi-Fi: not connected", RED)
        return [wifi_line,
                (f"IP address: {s['ip']}" if s["ip"] else "IP address: none", "FFFFFF" if s["ip"] else RED),
                ("Internet: online", GREEN) if s["internet"] else ("Internet: offline", RED)]

    # --- drawing -----------------------------------------------------------

    def _draw(self):
        try:
            if self.open:
                self.video.command("osd-overlay", OVERLAY_ID, "ass-events", self._render(),
                                   RES_X, RES_Y, 20)
            else:
                self.video.command("osd-overlay", OVERLAY_ID, "none", "")
        except Exception as e:
            print(f"debug menu: overlay failed: {e}", flush=True)

    def _render(self):
        out = [ass_rect(0, 0, RES_X, RES_Y, "000000", alpha="14")]
        title, rows, hint, body = "DEBUG MENU", None, "Up/Down choose   Enter select   Esc close", []

        if self.screen == "menu":
            rows = self._menu_items()
        elif self.screen == "networks":
            title = "Wi-Fi: choose a network"
            rows = [self._network_label(n) for n in self._network_rows()]
            hint = "Up/Down choose   Enter select   Esc back"
        elif self.screen in ("ssid", "password"):
            title = "Wi-Fi: network name" if self.screen == "ssid" else f"Wi-Fi: password for {self.target['ssid']}"
            shown = self.text if (self.screen == "ssid" or self.reveal) else "*" * len(self.text)
            body = [(shown + "_", "FFFFFF")]
            hint = "Type   Enter connect   Esc back" + ("   Tab show/hide" if self.screen == "password" else "")
            if self.screen == "ssid":
                hint = "Type   Enter next   Esc back"
        elif self.screen == "message":
            title = self.title
            body = [(l, "FFFFFF") for l in self.lines]
            hint = "Enter or Esc to go back"
        else:  # scanning / working
            title, hint = self.title, "Please wait..."

        out.append(ass_text(MARGIN, 70, escape_ass(title), 64, GOLD))
        y = 220
        if self.screen == "menu":
            y = 150
            for text, color in self._status_lines():
                out.append(ass_text(MARGIN, y, escape_ass(text), 36, color))
                y += 48
            y += 40
        if rows:
            first = max(0, min(self.sel - VISIBLE_ROWS // 2, len(rows) - VISIBLE_ROWS))
            for i in range(first, min(len(rows), first + VISIBLE_ROWS)):
                chosen = i == self.sel
                out.append(ass_text(MARGIN, y, escape_ass(("> " if chosen else "  ") + rows[i]), 44,
                                    GOLD if chosen else "FFFFFF"))
                y += ROW_H
        for text, color in body:
            out.append(ass_text(MARGIN, y, escape_ass(text), 44, color))
            y += ROW_H
        out.append(ass_text(MARGIN, RES_Y - 90, hint, 32, GREY))
        return "\n".join(out)

    @staticmethod
    def _network_label(net):
        if net.get("other"):
            return net["ssid"]
        bars = "#" * max(1, round(net["signal"] / 25))
        return f"{net['ssid']}   [{bars:<4}]" + ("  (locked)" if net["secured"] else "") + ("  (connected)" if net["active"] else "")
