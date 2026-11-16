#!/usr/bin/env python3
"""Overlord: fullscreen video/audio panel driven by an ESP32 over serial.

Events (one per line):
  video_once_<name>  play media/video_once/<name>.* once on the video, then back to the video loop
  audio_once_<name>  overlay media/audio_once/<name>.* on top of everything (overlaps freely)
  video_loop_<name>  switch the background video loop to media/video_loops/<name>.*
  audio_loop_<name>  switch the background audio loop to media/audio_loops/<name>.*
  audio_loop_off     stop the background audio loop
  audio_bed_<name>   a second background loop, under audio_loop_*, from media/audio_loops/<name>.*
                     (a cue for a bed already playing leaves it running)
  audio_bed_off      stop the bed
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
import time
from pathlib import Path

import keys
import pi_control
import players
import settings
from debug_menu import DebugMenu
from mpv_ipc import Mpv, MpvError, SoundPlayer
from ambient import Ambient, load as load_ambient
import gain as gain_config
from waveform import WaveformCache
from control import ControlServer
from serial_reader import SerialReader
from firmware_manager import FirmwareManager
from installation import InstallationManager

# On the Pi, media is synced into a directory outside the git checkout (see deploy/).
BUNDLED_MEDIA = Path(__file__).resolve().parent / "media"
MEDIA = Path(os.environ.get("OVERLORD_MEDIA") or BUNDLED_MEDIA)


def _has_media(root):
    try:
        return any(p.is_file() and not p.name.startswith(".")
                   for d in root.iterdir() if d.is_dir() for p in d.iterdir())
    except OSError:
        return False


# If the synced media is missing (a first boot with no internet, a lost disk),
# play the placeholder media in the checkout rather than a black screen.
if MEDIA != BUNDLED_MEDIA and not _has_media(MEDIA):
    print(f"no media in {MEDIA}; falling back to the bundled {BUNDLED_MEDIA}", flush=True)
    MEDIA = BUNDLED_MEDIA
ENTRY_BACKSTOP_S = 1.5  # after a state starts, replay entry videos that never arrived (entry_backstop)
MESSAGE_GRACE_S = 3.0   # a message this new survives a state change (see on_state_record)
CAPTION_ID = 12      # osd-overlay id for clip captions (goal band 8, messages 9, banners 11)
CAPTION_MAX_S = 20
DUCK_PREFIX = "janis"   # clips whose file starts so duck the loop and bed under them
DUCK_LEVEL = 0.7        # mpv volume is a cubic curve: 0.7 is about -9 dB   # a caption never outstays this, even if the clip's end is missed
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
    ap.add_argument("--serial-baud", type=int, default=int(os.environ.get("OVERLORD_SERIAL_BAUD", "921600")),
                    help="preferred bridge serial baud (default 921600; probes legacy 115200 too)")
    ap.add_argument("--control-port", type=int, default=8080,
                    help="port for the REST control API (0 = off; default 8080)")
    ap.add_argument("--control-bind", default=os.environ.get("OVERLORD_CONTROL_BIND", "0.0.0.0"),
                    help="dashboard/API bind address (default: all LAN interfaces, no login)")
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
                    gain_config.load(args.game or DEFAULT_GAME),
                    gain_config.load_ramps(args.game or DEFAULT_GAME))
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
        "audio_bed": None,
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
    # What Janis says in each clip, shown along the bottom while it plays
    # (captions.yaml beside the game file: clip name -> text).
    captions = {}
    goal_display = None   # set below, for a game with show_goals; told when clips start and end
    try:
        import yaml
        from game import DEFAULT_GAME as _game_path
        cap_path = Path(args.game or _game_path).with_name("captions.yaml")
        if cap_path.exists():
            captions = {str(k): str(v) for k, v in (yaml.safe_load(cap_path.read_text()) or {}).items()}
            print(f"captions: {len(captions)} from {cap_path}", flush=True)
    except Exception as e:
        print(f"captions: not loaded ({e})", flush=True)
    playlog = None
    sessions = None
    if not args.no_playlog:
        from playlog import PlayLog
        playlog = PlayLog()
        print(f"playlog: writing to {playlog.dir}", flush=True)
        if game:
            game.on_record(playlog.engine_record)
        from sessionlog import SessionLog, DEFAULT_IDLE
        states = list((game_cfg or {}).get("states") or {})
        sessions = SessionLog(playlog.dir / "activity.jsonl",
                              idle_state=(game_cfg or {}).get("start", DEFAULT_IDLE),
                              phases=states)
        playlog.listen(sessions.feed)

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
                    show_caption(None)
                    duck(False)
                    if goal_display:
                        goal_display.set_clip(False)
                    if state["video_loop"]:
                        video.play(state["video_loop"], loop=True)

    video = Mpv(video_extra, on_event=on_event, kiosk=True, name="video")
    audio = Mpv(extra, kiosk=False, name="audio")
    bed = Mpv(extra, kiosk=False, name="bed")  # audio_bed_*: plays under the audio loop

    # Escape opens the debug menu. In a window mpv sees the keys; on the panel
    # (DRM, no window) they are read from the keyboard devices instead.
    sounds = SoundPlayer(extra, volume=volume[0])

    def set_volume(percent):
        """Live on the running players and for every sound started later; remembered."""
        percent = max(0, min(100, percent))
        volume[0] = sounds.volume = percent
        for player in (audio, bed, video):
            ducked_now = player is not video and ducked[0]
            player.set("volume", percent * (DUCK_LEVEL if ducked_now else 1.0))
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
        bed.quit()
        video.quit()

    signal.signal(signal.SIGTERM, shutdown)

    gains = gain_config.GainStore()  # cue -> loudness multiplier, with live overrides; pointed at the game file below

    caption_timer = [None]

    def show_caption(text):
        """The clip's caption along the bottom of the video, or none. Cleared when
        the clip ends; CAPTION_MAX_S is only a backstop if that is never heard."""
        from simulate import RES_X, RES_Y, ass_rect
        if caption_timer[0]:
            caption_timer[0].cancel()
            caption_timer[0] = None
        try:
            if not text:
                video.command("osd-overlay", CAPTION_ID, "none", "")
                return
            esc = text.replace("\\", "\\\\").replace("{", "(").replace("}", ")")
            lines = [ass_rect(0, RES_Y - 190, RES_X, 190, "000000", alpha="50"),
                     f"{{\\an2\\pos({RES_X // 2},{RES_Y - 50})\\fs68\\bord3\\shad0\\b1\\q0"
                     f"\\1c&HFFFFFF&\\3c&H000000&}}{esc}"]
            video.command("osd-overlay", CAPTION_ID, "ass-events", "\n".join(lines), RES_X, RES_Y, 15)
            caption_timer[0] = threading.Timer(CAPTION_MAX_S, lambda: (show_caption(None), duck(False)))
            caption_timer[0].daemon = True
            caption_timer[0].start()
        except Exception as e:
            print(f"captions: overlay failed: {e}", flush=True)

    ducked = [False]

    def duck(on):
        """While Janis speaks, the loop and the bed drop to DUCK_LEVEL of the
        volume, so she is heard over them; back up when her clip ends."""
        if on == ducked[0]:
            return
        ducked[0] = on
        level = volume[0] * (DUCK_LEVEL if on else 1.0)
        for player in (audio, bed):
            try:
                player.set("volume", level)
            except Exception as e:
                print(f"duck: {e}", flush=True)

    def play_clip(path, interrupt=False, gain=1.0):
        """Play a clip once. Unless interrupt, a clip already playing wins and
        this one is dropped. Returns whether it played."""
        with lock:
            if state["playing_clip"] and not interrupt:
                return False
            state["playing_clip"] = True
            video.play(path, loop=False, gain=gain)
            show_caption(captions.get(Path(path).stem))
            duck(Path(path).stem.startswith(DUCK_PREFIX))
            if goal_display:
                goal_display.set_clip(True)
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

    def set_audio_bed(path, gain=1.0):
        with lock:
            if state["audio_bed"] == path:  # entering another state must not restart it
                return
            state["audio_bed"] = path
            bed.play(path, loop=True, gain=gain)

    def stop_audio_bed():
        with lock:
            state["audio_bed"] = None
            bed.stop()

    # (prefix, folder, action); longest prefixes first so they win.
    prefixed = [
        ("video_loop_", "video_loops", set_video_loop),
        ("audio_loop_", "audio_loops", set_audio_loop),
        ("audio_bed_", "audio_loops", set_audio_bed),
        ("video_once_", "video_once", play_clip),
        ("audio_once_", "audio_once", sounds.play),
    ]

    def cue_media(cue):
        """The media files a cue may play, sorted (for the Gain tab's waveforms)."""
        for prefix, folder, _action in prefixed:
            if cue.startswith(prefix):
                return players.media_files(MEDIA, folder, cue[len(prefix):], prefix)
        return []

    message_timer = [None]
    message_at = [None]   # when the message up now was shown, or None

    def show_message(name):
        """text_show_<name>: the game file's messages.<name> over the video for
        its `seconds`. Returns the name, or False if the game has no such message."""
        msg = ((game_cfg or {}).get("messages") or {}).get(name)
        if not msg:
            print(f"no message {name!r} under messages in the game file", flush=True)
            return False
        from simulate import RES_X, RES_Y, ass_rect, ass_text
        lines = [ass_rect(0, RES_Y // 2 - 150, RES_X, 300, "000000", alpha="40"),
                 ass_text(RES_X // 2, RES_Y // 2, msg["text"], 72, align=5)]
        video.command("osd-overlay", 9, "ass-events", "\n".join(lines), RES_X, RES_Y, 20)
        message_at[0] = time.monotonic()
        if message_timer[0]:
            message_timer[0].cancel()
        message_timer[0] = threading.Timer(msg["seconds"], clear_message)
        message_timer[0].daemon = True
        message_timer[0].start()
        return name

    def clear_message():
        if message_timer[0]:
            message_timer[0].cancel()
            message_timer[0] = None
        message_at[0] = None
        try:
            video.command("osd-overlay", 9, "none", "")
        except Exception as e:
            print(f"message: clear failed: {e}", flush=True)

    def entry_backstop(state, at):
        """The mesh can lose a cue (it once lost the jailbreak video). The state
        itself reaches us by another road, so ENTRY_BACKSTOP_S after a state
        starts, play any of its entry video cues that never arrived: a named
        video_once (the clip) and its video_loop. Wildcards and choose_one are
        left alone; Janis is not worth a second guess."""
        st = ((game_cfg or {}).get("states") or {}).get(state) or {}
        cues = ((st.get("on") or {}).get("enter") or {}).get("cue") or []
        for cue in cues:
            if not isinstance(cue, str) or "*" in cue or not cue.startswith(("video_once_", "video_loop_")):
                continue
            if cue_heard.get(cue, -1e9) >= at - 5.0:
                continue
            print(f"backstop: {state} entry cue {cue} never arrived; playing it", flush=True)
            fire_cue(cue, enter=True)

    def on_state_record(rec):
        """A new state takes down the last one's message. The cue that shows a
        message comes from a state's entry, and may reach us just before the
        state's own record does, so one only just shown is left up."""
        if rec.get("kind") == "state" and not rec.get("sync"):
            at = time.monotonic()
            t = threading.Timer(ENTRY_BACKSTOP_S, entry_backstop, args=(rec.get("name"), at))
            t.daemon = True
            t.start()
        if rec.get("kind") == "state" and message_at[0] is not None \
                and time.monotonic() - message_at[0] > MESSAGE_GRACE_S:
            clear_message()

    def dispatch(event_name, enter=False):
        """Play the cue. Returns what it played (the media file's name), None if
        the name is no cue, False if there is no media for it, or "skipped" for
        a clip dropped because another is playing. `enter` lets a clip interrupt."""
        if event_name == "audio_loop_off":
            stop_audio_loop()
            return "stopped"
        if event_name == "audio_bed_off":
            stop_audio_bed()
            return "stopped"
        if event_name.startswith("text_show_"):
            return show_message(event_name[len("text_show_"):])
        for prefix, folder, action in prefixed:
            if event_name.startswith(prefix):
                path = find_media(folder, event_name[len(prefix):], prefix)
                if path is None:
                    print(f"no media for {event_name} in media/{folder}/", flush=True)
                    return False
                if prefix.startswith("audio_"):
                    action(path, gains.get(event_name, 1.0))
                elif action is play_clip:
                    if not play_clip(path, interrupt=enter, gain=gains.get(event_name, 1.0)):
                        return "skipped"
                else:
                    action(path)
                return path.name
        return None

    cue_heard = {}   # cue name -> when it last reached fire_cue, for the entry backstop

    def fire_cue(cue, enter=False):
        """Play one cue; True if it played. `enter` marks a cue a game state
        fires on entry: its clip plays even if another clip is running."""
        cue_heard[cue] = time.monotonic()
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

    def pull_lever(channel):
        """A debug pull on a rope channel, as if its sensor had fired: through the
        bridge to the arbiter, or into overlord's own engine with --game. False
        if the bridge is not there."""
        if game:
            game.pull(channel)
        elif not serial.write_line(f"GAME-PULL {channel}"):
            return False
        print(f"lever: pull on channel {channel}", flush=True)
        return True

    def game_states():
        """State names of the game file, start first."""
        states = list((game_cfg or {}).get("states") or {})
        start = (game_cfg or {}).get("start")
        return sorted(states, key=lambda s: s != start)

    def force_state(name):
        """Debug: move the game to a state, through the bridge to the arbiter or
        into overlord's own engine with --game. False if the bridge is not there."""
        if game:
            game.goto(name)
        elif not serial.write_line(f"GAME-STATE {name}"):
            return False
        print(f"state: forced to {name}", flush=True)
        return True

    def control(method, parts):
        """The REST API (control.py). Returns (http status, body)."""
        if parts == ["status"] and method == "GET":
            return 200, {
                "volume": volume[0],
                "video_loop": state["video_loop"].name if state["video_loop"] else None,
                "audio_loop": state["audio_loop"].name if state["audio_loop"] else None,
                "audio_bed": state["audio_bed"].name if state["audio_bed"] else None,
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
        if len(parts) == 2 and parts[0] == "lever" and method in ("POST", "PUT"):
            try:
                channel = int(parts[1])
            except ValueError:
                channel = 0
            if not 1 <= channel <= 31:
                return 400, {"error": "lever must be a rope channel, 1 to 31"}
            if not pull_lever(channel):
                return 503, {"error": "the bridge is not connected"}
            return 200, {"channel": channel}
        if parts == ["states"] and method == "GET":
            return 200, {"states": game_states(), "current": game.state if game else None}
        if len(parts) == 2 and parts[0] == "state" and method in ("POST", "PUT"):
            if parts[1] not in game_states():
                return 404, {"error": f"the game has no state {parts[1]!r}"}
            if not force_state(parts[1]):
                return 503, {"error": "the bridge is not connected"}
            return 200, {"state": parts[1]}
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
    installation = None

    def on_serial_event(event_name):
        event_name = event_name.strip()
        if not event_name:
            return
        if installation and installation.handle_line(event_name):
            return
        # "PI restart overlord" from the mesh: the way back in when we are off the network.
        if pi_control.handle(event_name, args.player, lambda: video.command("quit")):
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
        if event_name.startswith("CO "):   # every component's charge, for the goal band
            if goal_display:
                goal_display.on_components(event_name)
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
        baud=args.serial_baud,
    )
    installation = InstallationManager(serial, busy=lambda: bool(
        firmware_updater and firmware_updater.snapshot()["active_job"]))
    serial.start()
    if not args.test:
        firmware_updater = FirmwareManager(serial)
        firmware_updater.start()
        installation.start()

    # A spinner in the corner while firmware is being flashed.
    if firmware_updater:
        from update_overlay import UpdateOverlay
        UpdateOverlay(video, firmware_updater.updating).start()

    if playlog:
        playlog.listen(on_state_record)

    # Goals over the video, for a game that wants them seen (show_goals: true).
    if playlog and game_cfg and game_cfg.get("show_goals"):
        from goal_display import GoalDisplay
        goal_display = GoalDisplay(game_cfg, video)
        playlog.listen(goal_display.on_record)

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
    gains.path = args.game or DEFAULT_GAME
    gains.reload()
    gains.media = cue_media
    wave_cache = WaveformCache(cue_media, settings.PATH.parent / "waveforms.json")

    def wave_cache_waves():
        return wave_cache.waves([cue for cue, _group in gain_config.cue_list(gains.path)])
    wave_cache_waves()   # start drawing them now, so the Gain tab is ready when opened
    ambient = None
    ambient_config = args.game or DEFAULT_GAME
    if ambient_config:
        ambient = Ambient(load_ambient(ambient_config), play_ambient)
        ambient.start()

    api = None
    if args.control_port:
        try:
            api = ControlServer(control, args.control_bind, args.control_port, firmware=firmware_updater, installation=installation,
                            sessions=sessions, gains=gains, waves=wave_cache_waves, playlog=playlog,
                            game_file=args.game or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "game_state_machine", "state_config.yaml"))
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
        if sessions:
            sessions.close()
        if ambient:
            ambient.stop()
        if api:
            api.stop()
        installation.stop()
        if firmware_updater:
            firmware_updater.stop()
        serial.stop()
        shutdown()


if __name__ == "__main__":
    main()
