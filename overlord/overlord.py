#!/usr/bin/env python3
"""Overlord: fullscreen video/audio panel driven by an ESP32 over serial.

Events (one per line):
  video_once_<name>  play media/video_once/<name>.* once on the video, then back to the video loop
  audio_once_<name>  overlay media/audio_once/<name>.* on top of everything (overlaps freely)
  video_loop_<name>  switch the background video loop to media/video_loops/<name>.*
  audio_loop_<name>  switch the background audio loop to media/audio_loops/<name>.*
  audio_loop_off     stop the background audio loop
  cue <name>         the arbiter's game fired a cue; play <name> as above (what the bridge sends)
  cue <name> enter   the same, fired on entering a game state

A video_once clip never cuts off a clip already playing, except one fired on
entering a state, which always plays: it marks a change of scene and the old
clip belongs to the old one. The skipped clip is dropped, not queued.

A * in a cue name picks one of the files it matches, at random, never the same
twice running: video_once_janis_nag_* plays one of the janis_nag_ clips.

A cue may be addressed to a player: "shrine__audio_once_bell" is for the Pi
named shrine and is ignored here; a cue with no prefix is the Overlord's. The
same program runs those other Pis as satellites (players.py) when
OVERLORD_PLAYER names one; only the Overlord runs the firmware updater.

Escape on the panel's keyboard opens a debug menu (Wi-Fi setup, network info, restart).

With --game, game events (component_activated, ...) drive the state machine in
game_state_machine/, which fires the cues above. --simulate adds keyboard
stand-ins for the components and an overlay showing their charge.
"""

import argparse
import os
import re
import signal
import sys
import threading
from pathlib import Path

import keys
import players
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
    """The file for a cue in media/<folder>/, or None (players.find_media)."""
    return players.find_media(MEDIA, folder, name, prefix)


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
    ap.add_argument("--player", default=os.environ.get("OVERLORD_PLAYER") or players.PRIMARY,
                    help="this Pi's player name (default $OVERLORD_PLAYER, else overlord);\n"
                         "any other name runs a satellite that plays only cues addressed to it")
    ap.add_argument("--player-kind", default=os.environ.get("OVERLORD_PLAYER_KIND") or "sound",
                    choices=sorted(players.KINDS),
                    help="what a satellite can play (default $OVERLORD_PLAYER_KIND, else sound)")
    ap.add_argument("--no-playlog", action="store_true",
                    help="do not keep the play log (overlord/playlog.py)")
    ap.add_argument("--director", action="store_true",
                    default=os.environ.get("OVERLORD_DIRECTOR", "") not in ("", "0"),
                    help="let the live director tune the game's params between sessions\n"
                         "(playtest/live.py; default $OVERLORD_DIRECTOR)")
    ap.add_argument("--director-prior", metavar="JSON",
                    default=os.environ.get("OVERLORD_DIRECTOR_PRIOR") or None,
                    help="a `python -m playtest tune --out` result to start the director from")
    ap.add_argument("--director-focus", metavar="PARAMS",
                    default=os.environ.get("OVERLORD_DIRECTOR_FOCUS") or None,
                    help="the params the director may turn, comma-separated (default: all\n"
                         "with a range; one or two at a time learns far faster)")
    args = ap.parse_args()
    if args.simulate and args.game is None:
        ap.error("--simulate needs --game")

    if args.player != players.PRIMARY:
        try:
            from game import DEFAULT_GAME
        except ImportError:  # pyyaml missing
            DEFAULT_GAME = None
        players.run(args, args.player, args.player_kind, MEDIA,
                    gain_config.load(args.game or DEFAULT_GAME))
        return

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
        game = Game(args.game or DEFAULT_GAME, lambda cue, enter=False: fire_cue(cue, enter))
    sim = None
    if args.simulate:
        from simulate import Simulator
        sim = Simulator(game, game.event)
        video_extra += sim.mpv_args()

    # The game file, for the play log's director and goal display. The arbiter
    # runs its own copy; this one should be the same file.
    game_cfg = None
    try:
        from game import DEFAULT_GAME as _default_game
        from playtest import spec as game_spec
        game_cfg = game_spec.load(args.game or _default_game)
    except (ImportError, OSError, ValueError) as e:
        print(f"playlog: no game file ({e}); no director or goal display", flush=True)
    playlog = None
    if not args.no_playlog:
        from playlog import PlayLog
        playlog = PlayLog()
        print(f"playlog: writing to {playlog.dir}", flush=True)
        if game:
            game.on_record(playlog.engine_record)

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

    def play_clip(path, interrupt=False):
        """Play a clip once. Unless interrupt, a clip already playing wins and
        this one is dropped. Returns whether it played."""
        with lock:
            if state["playing_clip"] and not interrupt:
                return False
            state["playing_clip"] = True
            video.play(path, loop=False)
            return True

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

    def dispatch(event_name, enter=False):
        """Play the cue. Returns what it played (the media file's name), None if
        the name is no cue, False if there is no media for it, or "skipped" for
        a clip dropped because another is playing. `enter` lets a clip interrupt."""
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
                elif action is play_clip:
                    if not play_clip(path, interrupt=enter):
                        return "skipped"
                else:
                    action(path)
                return path.name
        return None

    def fire_cue(cue, enter=False):
        """Play one cue; True if it played. `enter` marks a cue a game state
        fires on entry: its clip plays even if another clip is running."""
        # Game-control cues are for the game system, not overlord media.
        if cue.startswith("game_"):
            return False
        # A cue for another player is that Pi's; "overlord__..." is ours.
        player, bare = players.split_target(cue)
        if player != players.PRIMARY:
            return False
        cue = bare
        try:
            played = dispatch(cue, enter)
            if played == "skipped":
                print(f"cue skipped, a clip is playing: {cue}", flush=True)
                return False
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
            if fire_cue(parts[1], enter=True):   # an operator's cue always plays
                return 200, {"cue": parts[1]}
            return 404, {"error": f"cue not played: {parts[1]} (see the log)"}
        if len(parts) == 3 and parts[0] == "param" and method in ("POST", "PUT"):
            # Tune by hand without a reflash: the arbiter clamps it to the game's range.
            try:
                value = parse_param_value(parts[2])
            except ValueError:
                return 400, {"error": f"value must look like 90, 1.5 or 30s, not {parts[2]!r}"}
            if not send_param(parts[1], value):
                return 503, {"error": "no bridge connected to send it through"}
            if director:
                director.choice[parts[1]] = value   # credit the next session to what is live
            return 200, {"param": parts[1], "value": value}
        if parts[:1] == ["playtest"] and method == "GET":
            return 200, {"playlog": playlog.path() if playlog else None,
                         "records": playlog.records if playlog else 0,
                         "director": director.summary() if director else "off"}
        if parts in (["playtest", "freeze"], ["playtest", "thaw"]) and method in ("POST", "PUT"):
            if not director:
                return 404, {"error": "the director is off (--director)"}
            director.frozen = parts[1] == "freeze"
            return 200, {"frozen": director.frozen}
        return 404, {"error": "no such route",
                     "routes": ["GET /status", "GET /volume", "POST /volume/<80|+10|-10>",
                                "POST /cue/<name>", "POST /param/<name>/<value>",
                                "GET /playtest", "POST /playtest/freeze", "POST /playtest/thaw"]}

    def parse_param_value(text):
        """90, 1.5 or a duration (30s, 2m) in seconds."""
        if text[-1:] in ("s", "m"):
            return float(text[:-1]) * (60 if text[-1] == "m" else 1)
        return float(text)

    def send_param(name, value):
        """One param to the game: overlord's own engine with --game, else the
        arbiter, through the bridge (it relays GAME-PARAM over the mesh)."""
        if game:
            try:
                game.set_param(name, value)
                return True
            except KeyError:
                return False
        return serial.write_line(f"GAME-PARAM {name} {value:g}")

    director = None

    firmware_updater = None

    def on_serial_event(event_name):
        event_name = event_name.strip()
        if not event_name:
            return
        if firmware_updater and firmware_updater.handle_line(event_name):
            return
        # Every pull and cue goes in the play log. With --game, overlord's own
        # engine writes the log instead, so the bridge's lines would be doubles.
        if playlog and not game:
            playlog.line(event_name)
        # The bridge's lines: "cue <name>" is the arbiter's game firing a cue,
        # "EV ..." a raw sensor event the arbiter has already turned into game
        # events, so there is nothing more for us to do with it.
        parsed = players.parse_cue_line(event_name)
        if parsed:
            fire_cue(*parsed)
            return
        if event_name.startswith(("EV ", "GI ")):   # a pull; the game's telemetry
            return
        # A bridge on USB shares its port with its console, so its banner and
        # status lines arrive here too. An event is one bare word; anything
        # else is the bridge talking, worth a log line and nothing more.
        if not re.fullmatch(r"[a-z0-9_*-]+", event_name):
            print(f"bridge: {event_name}", flush=True)
            return
        if game and game.handles(event_name):
            game.event(event_name)
            return
        try:
            played = dispatch(event_name)
            if played == "skipped":
                print(f"event skipped, a clip is playing: {event_name}", flush=True)
            elif played:
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

    # Goals over the video, for a game that wants them seen (show_goals: true).
    if playlog and game_cfg and game_cfg.get("show_goals"):
        from goal_display import GoalDisplay
        playlog.listen(GoalDisplay(game_cfg, video).on_record)

    # The live director learns from each session which params people enjoy
    # most, and sets them for the next (playtest/live.py).
    if args.director and playlog and game_cfg:
        import json as _json
        import time as _time
        from playtest.live import Director
        prior = None
        if args.director_prior:
            with open(args.director_prior) as f:
                prior = _json.load(f).get("prior")
        state_path = str(playlog.dir / "director.json")
        playlog.dir.mkdir(parents=True, exist_ok=True)
        focus = [p.strip() for p in args.director_focus.split(",")] if args.director_focus else None
        director = Director(game_cfg, send=lambda n, v: send_param(n, v), state_path=state_path,
                            prior=prior, focus=focus, log=lambda m: print(m, flush=True))
        playlog.listen(director.feed)

        def director_clock():
            while True:
                _time.sleep(1.0)
                try:
                    director.tick(_time.time())
                except Exception as e:  # the show goes on without it
                    print(f"director: {e}", flush=True)

        threading.Thread(target=director_clock, daemon=True).start()
        print(f"director: tuning {', '.join(director.arms)}; state in {state_path}", flush=True)

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
        if playlog:
            playlog.close()
        if ambient:
            ambient.stop()
        if api:
            api.stop()
        serial.stop()
        shutdown()


if __name__ == "__main__":
    main()
