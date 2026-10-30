"""Players: the Pis that play the game's cues, and how a cue finds its Pi.

Every Pi in the piece runs this checkout and has its own bridge (an ESP32-C3 on
its UART), so every Pi hears every cue the arbiter's game fires. A cue says
which Pi it is for by a prefix:

    shrine__audio_once_bell     played by the player named "shrine"
    audio_once_chime            no prefix: played by the Overlord

The Overlord (overlord.py) is the primary player: the video panel, ambient
sounds, the local game simulator, and the one firmware updater for the whole
mesh. Everything else is a satellite, one of the classes below, and does none
of that: it plays the cues addressed to it and nothing more. A satellite's
bridge never hosts the firmware AP either -- only the Overlord's updater asks
for it (src/bridge).

A Pi is told what it is in /etc/overlord.env:

    OVERLORD_PLAYER=shrine          # the name cues address; unset = overlord
    OVERLORD_PLAYER_KIND=image      # sound (default) | image

Media comes from the same synced folder as the Overlord's. A satellite looks
in media/<player>/<folder>/ first, then media/<folder>/, so a file meant for one
Pi can sit apart from everyone else's, and a shared one can be used by all.
"""

import signal
import threading
from pathlib import Path

from mpv_ipc import Mpv, MpvError, SoundPlayer

PRIMARY = "overlord"
TARGET_SEP = "__"

# What the part after a prefix may be: the cue kinds some player can play.
# Keep in step with PLAYER_KINDS in tools/game_to_json.py.
CUE_KINDS = ("audio_once_", "audio_loop_", "video_once_", "video_loop_", "image_show_")
CUE_WORDS = ("audio_loop_off", "image_off")


def split_target(cue):
    """("shrine", "audio_once_bell") for "shrine__audio_once_bell";
    (PRIMARY, cue) for a cue with no prefix."""
    if TARGET_SEP in cue:
        player, bare = cue.split(TARGET_SEP, 1)
        return player, bare
    return PRIMARY, cue


def find_media(root, folder, name, prefix="", player=None):
    """The file for a cue in <root>/[<player>/]<folder>/, or None. Rejects path tricks.

    The file may carry the whole cue name (audio_once_chime.mp3, the form that
    matches what the game config says) or just the part after the prefix
    (chime.mp3). The full name wins if both exist. Any extension. A player's own
    folder wins over the shared one."""
    if not name or "/" in name or "\\" in name or name.startswith("."):
        return None
    dirs = ([Path(root) / player / folder] if player else []) + [Path(root) / folder]
    for d in dirs:
        for stem in ([prefix + name] if prefix else []) + [name]:
            matches = sorted(p for p in d.glob(f"{stem}.*") if p.is_file())
            if matches:
                return matches[0]
    return None


class Satellite:
    """One Pi that plays the cues addressed to it by name. Subclasses add what
    it can play by extending handlers(); this holds the routing, the volume, and
    the REST status the Overlord's control API also has."""

    kind = "base"

    def __init__(self, name, media, mpv_args=(), gains=None, volume=100):
        self.name = name
        self.media = Path(media)
        self.mpv_args = list(mpv_args)
        self.gains = gains or {}
        self.volume = volume
        self.lock = threading.RLock()

    # -- what it plays ------------------------------------------------------
    def handlers(self):
        """[(prefix, folder, action)]: a cue starting with prefix plays the file
        for the rest of its name from folder. Longest prefixes first."""
        return []

    def words(self):
        """{cue: action} for cues that are a whole word, like audio_loop_off."""
        return {}

    def start(self):
        pass

    def stop(self):
        pass

    def wait(self):
        """Block for the life of the player."""
        threading.Event().wait()

    def status(self):
        return {"player": self.name, "kind": self.kind, "volume": self.volume}

    def set_volume(self, percent):
        import settings
        self.volume = max(0, min(100, percent))
        settings.save(volume=self.volume)

    # -- routing ------------------------------------------------------------
    def gain(self, cue, bare):
        return self.gains.get(cue, self.gains.get(bare, 1.0))

    def fire_cue(self, cue):
        """Play a cue if it is addressed to this player. True if it played."""
        player, bare = split_target(cue)
        if player != self.name:
            return False
        try:
            word = self.words().get(bare)
            if word:
                word()
                print(f"cue: {cue}", flush=True)
                return True
            for prefix, folder, action in self.handlers():
                if not bare.startswith(prefix):
                    continue
                path = find_media(self.media, folder, bare[len(prefix):], prefix, self.name)
                if path is None:
                    print(f"cue {cue}: no media in {self.name}/{folder}/ or {folder}/", flush=True)
                    return False
                action(path, self.gain(cue, bare))
                print(f"cue: {cue} ({path.name})", flush=True)
                return True
            print(f"cue {cue}: this {self.kind} player cannot play that kind of cue", flush=True)
        except MpvError as e:
            print(f"cue {cue} failed: {e}", flush=True)
        return False

    def on_line(self, line):
        """A line from the bridge. Only cues matter here: sensor events are the
        arbiter's to turn into a game, and FW- lines belong to the Overlord."""
        line = line.strip()
        if line.startswith("cue "):
            self.fire_cue(line[4:].strip())

    def control(self, method, parts):
        """The REST API, the Overlord's routes that make sense here."""
        if parts == ["status"] and method == "GET":
            return 200, self.status()
        if parts[:1] == ["volume"]:
            if parts == ["volume"] and method == "GET":
                return 200, {"volume": self.volume}
            if len(parts) == 2 and method in ("POST", "PUT"):
                text = parts[1]
                try:
                    level = int(text) + (self.volume if text[:1] in "+-" else 0)
                except ValueError:
                    return 400, {"error": f"volume must look like 80, +10 or -10, not {text!r}"}
                self.set_volume(level)
                return 200, {"volume": self.volume}
        if len(parts) == 2 and parts[0] == "cue" and method in ("POST", "PUT"):
            # A bare name is taken as this player's, so curl needs no prefix.
            cue = parts[1] if TARGET_SEP in parts[1] else f"{self.name}{TARGET_SEP}{parts[1]}"
            if self.fire_cue(cue):
                return 200, {"cue": cue}
            return 404, {"error": f"cue not played: {cue} (see the log)"}
        return 404, {"error": "no such route",
                     "routes": ["GET /status", "GET /volume", "POST /volume/<80|+10|-10>",
                                "POST /cue/<name>"]}


class SoundSatellite(Satellite):
    """A speaker: one-shot sounds over everything, and a background loop."""

    kind = "sound"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.sounds = None
        self.audio = None
        self.audio_loop = None

    def start(self):
        extra = [f"--volume={self.volume}", *self.mpv_args]
        self.sounds = SoundPlayer(extra, volume=self.volume)
        self.audio = Mpv(extra, kiosk=False, name=f"{self.name}-audio")
        idle = find_media(self.media, "audio_loops", "idle", "audio_loop_", self.name)
        if idle:
            self.set_audio_loop(idle, self.gains.get("audio_loop_idle", 1.0))

    def stop(self):
        if self.sounds:
            self.sounds.stop_all()
        if self.audio:
            self.audio.quit()

    def handlers(self):
        return [
            ("audio_loop_", "audio_loops", self.set_audio_loop),
            ("audio_once_", "audio_once", lambda path, gain: self.sounds.play(path, gain)),
        ]

    def words(self):
        return {"audio_loop_off": self.stop_audio_loop}

    def set_audio_loop(self, path, gain=1.0):
        with self.lock:
            self.audio_loop = path
            self.audio.play(path, loop=True, gain=gain)

    def stop_audio_loop(self):
        with self.lock:
            self.audio_loop = None
            self.audio.stop()

    def set_volume(self, percent):
        super().set_volume(percent)
        if self.sounds:
            self.sounds.volume = self.volume
        if self.audio:
            self.audio.set("volume", self.volume)

    def status(self):
        return {**super().status(),
                "audio_loop": self.audio_loop.name if self.audio_loop else None}


class ImageSatellite(SoundSatellite):
    """A still-image display, full screen, that can also play sounds.

        image_show_<name>   show media/[<player>/]images/<name>.* until the next one
        image_off           go to black
    """

    kind = "image"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.screen = None
        self.image = None

    def start(self):
        super().start()
        # An image held indefinitely, not a one-frame "video" that ends.
        self.screen = Mpv([f"--volume={self.volume}", "--image-display-duration=inf",
                           *self.mpv_args], kiosk=True, name=f"{self.name}-image")
        idle = find_media(self.media, "images", "idle", "image_show_", self.name)
        if idle:
            self.show(idle)

    def stop(self):
        super().stop()
        if self.screen:
            self.screen.quit()

    def wait(self):
        self.screen.proc.wait()   # the screen going away is the player going away

    def handlers(self):
        return [("image_show_", "images", lambda path, _gain: self.show(path)),
                *super().handlers()]

    def words(self):
        return {**super().words(), "image_off": self.blank}

    def show(self, path):
        with self.lock:
            self.image = path
            self.screen.play(path, loop=True)

    def blank(self):
        with self.lock:
            self.image = None
            self.screen.stop()

    def status(self):
        return {**super().status(), "image": self.image.name if self.image else None}


KINDS = {"sound": SoundSatellite, "image": ImageSatellite}


def run(args, name, kind, media, gains):
    """Run a satellite until it is stopped. `args` are overlord.py's parsed
    arguments: the same flags work, minus the Overlord-only ones."""
    from control import ControlServer
    from serial_reader import SerialReader
    import settings

    if kind not in KINDS:
        raise SystemExit(f"OVERLORD_PLAYER_KIND={kind!r}: use one of {', '.join(KINDS)}")
    if name == PRIMARY or TARGET_SEP in name:
        raise SystemExit(f"a satellite cannot be called {name!r}")
    player = KINDS[kind](name, media, args.mpv_arg, gains,
                         volume=settings.load().get("volume", 100))
    print(f"satellite: {name} ({kind}) — playing cues addressed {name}{TARGET_SEP}…", flush=True)

    def stop(*_):
        player.stop()
    signal.signal(signal.SIGTERM, stop)

    player.start()
    serial = SerialReader(on_event=player.on_line, test_mode=args.test, port=args.serial_port)
    serial.start()
    api = None
    if args.control_port:
        try:
            api = ControlServer(player.control, args.control_bind, args.control_port)
            api.start()
            print(f"control: http://{api.address[0]}:{api.address[1]}/status", flush=True)
        except OSError as e:
            print(f"control: could not listen on {args.control_bind}:{args.control_port}: {e}",
                  flush=True)
    try:
        player.wait()
    except KeyboardInterrupt:
        pass
    finally:
        if api:
            api.stop()
        serial.stop()
        player.stop()
