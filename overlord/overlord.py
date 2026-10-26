#!/usr/bin/env python3
"""Overlord: fullscreen video/audio panel driven by an ESP32 over serial.

Events (one per line):
  video_once_<name>  play media/video_once/<name>.* once on the video, then back to the video loop
  audio_once_<name>  overlay media/audio_once/<name>.* on top of everything (overlaps freely)
  video_loop_<name>  switch the background video loop to media/video_loops/<name>.*
  audio_loop_<name>  switch the background audio loop to media/audio_loops/<name>.*
  audio_loop_off     stop the background audio loop
  cue <name>         the arbiter's game fired a cue; play <name> as above (what the bridge sends)

Escape on the panel's keyboard opens a debug menu (Wi-Fi setup, network info, restart).

With --game, game events (component_activated, ...) drive the state machine in
game_state_machine/, which fires the cues above. --simulate adds keyboard
stand-ins for the components and an overlay showing their charge.
"""

import argparse
import os
import signal
import sys
import threading
from pathlib import Path

import keys
import settings
from debug_menu import DebugMenu
from mpv_ipc import Mpv, MpvError, SoundPlayer
from ambient import Ambient, load as load_ambient
import gain as gain_config
from control import ControlServer
from serial_reader import SerialReader
from firmware import FirmwareUpdater

# On the Pi, media is synced into a directory outside the git checkout (see deploy/).
MEDIA = Path(os.environ.get("OVERLORD_MEDIA") or Path(__file__).resolve().parent / "media")
DEFAULT_LOOP = "idle"  # played at startup from video_loops/ and audio_loops/ if present


def find_media(folder, name, prefix=""):
    """The file for a cue in media/<folder>/, or None. Rejects path tricks.

    The file may carry the whole cue name (audio_once_chime.mp3, the form that
    matches what the game config says) or just the part after the prefix
    (chime.mp3). The full name wins if both exist. Any extension."""
    if not name or "/" in name or "\\" in name or name.startswith("."):
        return None
    for stem in ([prefix + name] if prefix else []) + [name]:
        matches = sorted(p for p in (MEDIA / folder).glob(f"{stem}.*") if p.is_file())
        if matches:
            return matches[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--windowed", action="store_true", help="dev: run video in a window")
    ap.add_argument("--mpv-arg", action="append", default=[],
                    help="extra mpv arg for every player (repeatable)")
    ap.add_argument("--test", action="store_true",
                    help="test mode: listen on $OVERLORD_SOCK (default /tmp/overlord.sock)")
    ap.add_argument("--serial-port", help="explicit serial port path")
    ap.add_argument("--control-port", type=int, default=8080,
                    help="port for the REST control API (0 = off; default 8080)")
    ap.add_argument("--control-bind", default="127.0.0.1",
                    help="address the control API listens on; it has no authentication,\n"
                         "so the default is this machine only (use 0.0.0.0 for the network)")
    ap.add_argument("--game", nargs="?", const="", metavar="YAML",
                    help="run a game state machine (default: game_state_machine/state_config.yaml)")
    ap.add_argument("--simulate", action="store_true",
                    help="keys 1-8 stand in for the components; needs --game")
    args = ap.parse_args()
    if args.simulate and args.game is None:
        ap.error("--simulate needs --game")

    volume = [settings.load().get("volume", 100)]  # percent; mpv's 100 is unity gain
    extra = [f"--volume={volume[0]}", *args.mpv_arg]  # --mpv-arg=--volume=N still wins at start
    video_extra = list(extra)
    if args.windowed:
        video_extra += ["--no-fullscreen", "--border", "--geometry=960x540"]

    # Touched from the serial thread and the mpv event thread.
    lock = threading.RLock()
    state = {
        "video_loop": find_media("video_loops", DEFAULT_LOOP, "video_loop_"),
        "audio_loop": find_media("audio_loops", DEFAULT_LOOP, "audio_loop_"),
        "playing_clip": False,
    }

    # Loaded before mpv starts so a broken game file fails fast. Optional modules
    # are imported only when asked for.
    game = None
    if args.game is not None:
        from game import DEFAULT_GAME, Game
        game = Game(args.game or DEFAULT_GAME, lambda cue: fire_cue(cue))
    sim = None
    if args.simulate:
        from simulate import Simulator
        sim = Simulator(game, game.event)
        video_extra += sim.mpv_args()

    menu = None  # set once the video player exists
    just_loaded = [False]  # a playback-restart straight after file-loaded is the start, not a loop

    def on_event(ev):
        args = ev.get("args") or []
        if ev.get("event") == "client-message" and args[:1] == ["dbg-key"] and len(args) == 2:
            menu.on_key(keys.from_mpv(args[1]))
        elif sim and not (menu and menu.open):  # typing in the menu must not light components
            sim.on_mpv_event(ev)
        if ev["event"] in ("file-loaded", "end-file"):
            print(f"mpv: {ev['event']}", ev.get("reason", ""), flush=True)

        # A looping file never ends or reloads, so the only sign of a new pass is
        # playback-restart. It also follows every file-loaded; that one is not a loop.
        if ev["event"] == "file-loaded":
            just_loaded[0] = True
        elif ev["event"] == "playback-restart":
            if just_loaded[0]:
                just_loaded[0] = False
            else:
                print("video: loop", flush=True)

        # Only a natural end counts: "stop" also fires when a clip replaces the loop.
        if ev["event"] == "end-file" and ev.get("reason") == "eof":
            with lock:
                if state["playing_clip"]:
                    print("clip finished, returning to loop", flush=True)
                    state["playing_clip"] = False
                    if state["video_loop"]:
                        video.play(state["video_loop"], loop=True)

    video = Mpv(video_extra, on_event=on_event, kiosk=True, name="video")
    audio = Mpv(extra, kiosk=False, name="audio")

    # Escape opens the debug menu. In a window mpv sees the keys; on the panel
    # (DRM, no window) they are read from the keyboard devices instead.
    sounds = SoundPlayer(extra, volume=volume[0])

    def set_volume(percent):
        """Live on the running players and for every sound started later; remembered."""
        percent = max(0, min(100, percent))
        volume[0] = sounds.volume = percent
        for player in (audio, video):
            player.set("volume", percent)
        settings.save(volume=percent)

    def test_sound():
        path = (find_media("audio_once", "chime", "audio_once_")
                    or find_media("audio_once", "beep", "audio_once_"))
        if path:
            sounds.play(path)

    menu = DebugMenu(video, restart=lambda: video.command("quit"),
                     volume=(lambda: volume[0], set_volume), test_sound=test_sound)
    keyboard = None
    if sys.platform.startswith("linux") and not args.windowed:
        keyboard = keys.EvdevKeys(menu.on_key)
        keyboard.start()
    else:
        for mpv_key, arg in keys.mpv_bindings():
            video.command("keybind", mpv_key, f"script-message dbg-key {arg}")

    def shutdown(*_):
        if keyboard:
            keyboard.stop()
        if sim:
            sim.stop()
        if game:
            game.stop()
        sounds.stop_all()
        audio.quit()
        video.quit()

    signal.signal(signal.SIGTERM, shutdown)

    gains = {}  # cue name -> loudness multiplier; filled below once the game file is known

    def play_clip(path):
        with lock:
            state["playing_clip"] = True
            video.play(path, loop=False)

    def set_video_loop(path):
        with lock:
            state["video_loop"] = path
            if not state["playing_clip"]:  # a running clip hands back to the new loop
                video.play(path, loop=True)

    def set_audio_loop(path, gain=1.0):
        with lock:
            state["audio_loop"] = path
            audio.play(path, loop=True, gain=gain)

    def stop_audio_loop():
        with lock:
            state["audio_loop"] = None
            audio.stop()

    # (prefix, folder, action); longest prefixes first so they win.
    prefixed = [
        ("video_loop_", "video_loops", set_video_loop),
        ("audio_loop_", "audio_loops", set_audio_loop),
        ("video_once_", "video_once", play_clip),
        ("audio_once_", "audio_once", sounds.play),
    ]

    def dispatch(event_name):
        """Play the cue. Returns what it played (the media file's name), or None."""
        if event_name == "audio_loop_off":
            stop_audio_loop()
            return "stopped"
        for prefix, folder, action in prefixed:
            if event_name.startswith(prefix):
                path = find_media(folder, event_name[len(prefix):], prefix)
                if path is None:
                    print(f"no media for {event_name} in media/{folder}/", flush=True)
                    return False
                if prefix.startswith("audio_"):
                    action(path, gains.get(event_name, 1.0))
                else:
                    action(path)
                return path.name
        return None

    def fire_cue(cue):
        """Play one cue; True if it played."""
        # Game-control cues are for the game system, not overlord media.
        if cue.startswith("game_"):
            return False
        try:
            played = dispatch(cue)
            if played:
                print(f"cue: {cue} ({played})", flush=True)
                return True
            print(f"cue not played: {cue}", flush=True)
        except MpvError as e:
            print(f"cue {cue} failed: {e}", flush=True)
        return False

    def control(method, parts):
        """The REST API (control.py). Returns (http status, body)."""
        if parts == ["status"] and method == "GET":
            return 200, {
                "volume": volume[0],
                "video_loop": state["video_loop"].name if state["video_loop"] else None,
                "audio_loop": state["audio_loop"].name if state["audio_loop"] else None,
                "playing_clip": state["playing_clip"],
                "game_state": game.state if game else None,
            }
        if parts[:1] == ["volume"]:
            if parts == ["volume"] and method == "GET":
                return 200, {"volume": volume[0]}
            if len(parts) == 2 and method in ("POST", "PUT"):
                text = parts[1]
                try:
                    # "80" sets it; "+10" and "-10" move it from where it is.
                    level = int(text) + (volume[0] if text[:1] in "+-" else 0)
                except ValueError:
                    return 400, {"error": f"volume must look like 80, +10 or -10, not {text!r}"}
                set_volume(level)
                print(f"volume: {volume[0]}%", flush=True)
                return 200, {"volume": volume[0]}
        if len(parts) == 2 and parts[0] == "cue" and method in ("POST", "PUT"):
            if fire_cue(parts[1]):
                return 200, {"cue": parts[1]}
            return 404, {"error": f"cue not played: {parts[1]} (see the log)"}
        return 404, {"error": "no such route",
                     "routes": ["GET /status", "GET /volume", "POST /volume/<80|+10|-10>",
                                "POST /cue/<name>"]}

    firmware_updater = None

    def on_serial_event(event_name):
        event_name = event_name.strip()
        if not event_name:
            return
        if firmware_updater and firmware_updater.handle_line(event_name):
            return
        # The bridge's lines: "cue <name>" is the arbiter's game firing a cue,
        # "EV ..." a raw sensor event the arbiter has already turned into game
        # events, so there is nothing for us to do with it.
        if event_name.startswith("cue "):
            fire_cue(event_name[4:].strip())
            return
        if event_name.startswith("EV "):
            return
        if game and game.handles(event_name):
            game.event(event_name)
            return
        try:
            if dispatch(event_name):
                print(f"event: {event_name}", flush=True)
            else:
                print(f"unknown event: {event_name}", flush=True)
        except MpvError as e:
            print(f"event {event_name} failed: {e}", flush=True)

    serial = SerialReader(
        on_event=on_serial_event,
        test_mode=args.test,
        port=args.serial_port,
    )
    serial.start()
    if not args.test:
        firmware_updater = FirmwareUpdater(serial)
        firmware_updater.start()

    # Sounds on a schedule, from the game YAML. Independent of --game and of
    # whatever the arbiter is doing: overlord is the only one that reads them.
    def play_ambient(name):
        if not name.startswith("audio_once_"):
            return False
        path = find_media("audio_once", name[len("audio_once_"):], "audio_once_")
        if path is None:
            return False
        sounds.play(path, gains.get(name, 1.0))
        return True

    try:
        from game import DEFAULT_GAME
    except ImportError:  # pyyaml missing: game.py cannot be imported
        DEFAULT_GAME = None
    gains.update(gain_config.load(args.game or DEFAULT_GAME))
    ambient = None
    ambient_config = args.game or DEFAULT_GAME
    if ambient_config:
        ambient = Ambient(load_ambient(ambient_config), play_ambient)
        ambient.start()

    api = None
    if args.control_port:
        try:
            api = ControlServer(control, args.control_bind, args.control_port)
            api.start()
            print(f"control: http://{api.address[0]}:{api.address[1]}/status", flush=True)
        except OSError as e:  # the player matters more than its remote control
            print(f"control: could not listen on {args.control_bind}:{args.control_port}: {e}",
                  flush=True)

    try:
        if state["audio_loop"]:
            audio.play(state["audio_loop"], loop=True,
                       gain=gains.get(f"audio_loop_{DEFAULT_LOOP}", 1.0))
            print(f"starting audio loop: {state['audio_loop'].name}", flush=True)
        if state["video_loop"]:
            video.play(state["video_loop"], loop=True)
            print(f"starting idle loop: {state['video_loop'].name}", flush=True)
        else:
            print(f"no media/video_loops/{DEFAULT_LOOP}.* found; showing black", flush=True)
        if game:
            game.start()
        if sim:
            sim.start(video)
        video.proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        if ambient:
            ambient.stop()
        if api:
            api.stop()
        serial.stop()
        shutdown()


if __name__ == "__main__":
    main()
