#!/usr/bin/env python3
"""Overlord: fullscreen video/audio panel driven by an ESP32 over serial.

Events (one per line):
  video_once_<name>  play media/video_once/<name>.* once on the video, then back to the video loop
  audio_once_<name>  overlay media/audio_once/<name>.* on top of everything (overlaps freely)
  video_loop_<name>  switch the background video loop to media/video_loops/<name>.*
  audio_loop_<name>  switch the background audio loop to media/audio_loops/<name>.*
  audio_loop_off     stop the background audio loop

With --game, game events (component_activated, ...) drive the state machine in
game_state_machine/, which fires the cues above. --simulate adds keyboard
stand-ins for the components and an overlay showing their charge.
"""

import argparse
import os
import signal
import threading
from pathlib import Path

from mpv_ipc import Mpv, MpvError, SoundPlayer
from serial_reader import SerialReader

# On the Pi, media is synced into a directory outside the git checkout (see deploy/).
MEDIA = Path(os.environ.get("OVERLORD_MEDIA") or Path(__file__).resolve().parent / "media")
DEFAULT_LOOP = "idle"  # played at startup from video_loops/ and audio_loops/ if present


def find_media(folder, name):
    """media/<folder>/<name>.* (any extension), or None. Rejects path tricks."""
    if not name or "/" in name or "\\" in name or name.startswith("."):
        return None
    matches = sorted(p for p in (MEDIA / folder).glob(f"{name}.*") if p.is_file())
    return matches[0] if matches else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--windowed", action="store_true", help="dev: run video in a window")
    ap.add_argument("--mpv-arg", action="append", default=[],
                    help="extra mpv arg for every player (repeatable)")
    ap.add_argument("--test", action="store_true",
                    help="test mode: listen on $OVERLORD_SOCK (default /tmp/overlord.sock)")
    ap.add_argument("--serial-port", help="explicit serial port path")
    ap.add_argument("--game", nargs="?", const="", metavar="YAML",
                    help="run a game state machine (default: game_state_machine/state_config.yaml)")
    ap.add_argument("--simulate", action="store_true",
                    help="keys 1-8 stand in for the components; needs --game")
    args = ap.parse_args()
    if args.simulate and args.game is None:
        ap.error("--simulate needs --game")

    extra = list(args.mpv_arg)
    video_extra = list(extra)
    if args.windowed:
        video_extra += ["--no-fullscreen", "--border", "--geometry=960x540"]

    # Touched from the serial thread and the mpv event thread.
    lock = threading.RLock()
    state = {
        "video_loop": find_media("video_loops", DEFAULT_LOOP),
        "audio_loop": find_media("audio_loops", DEFAULT_LOOP),
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

    def on_event(ev):
        if sim:
            sim.on_mpv_event(ev)
        if ev["event"] in ("file-loaded", "end-file"):
            print(f"mpv: {ev['event']}", ev.get("reason", ""), flush=True)

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
    sounds = SoundPlayer(extra)

    def shutdown(*_):
        if sim:
            sim.stop()
        if game:
            game.stop()
        sounds.stop_all()
        audio.quit()
        video.quit()

    signal.signal(signal.SIGTERM, shutdown)

    def play_clip(path):
        with lock:
            state["playing_clip"] = True
            video.play(path, loop=False)

    def set_video_loop(path):
        with lock:
            state["video_loop"] = path
            if not state["playing_clip"]:  # a running clip hands back to the new loop
                video.play(path, loop=True)

    def set_audio_loop(path):
        with lock:
            state["audio_loop"] = path
            audio.play(path, loop=True)

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
        if event_name == "audio_loop_off":
            stop_audio_loop()
            return True
        for prefix, folder, action in prefixed:
            if event_name.startswith(prefix):
                path = find_media(folder, event_name[len(prefix):])
                if path is None:
                    print(f"no media for {event_name} in media/{folder}/", flush=True)
                    return False
                action(path)
                return True
        return False

    def fire_cue(cue):
        try:
            if not dispatch(cue):
                print(f"cue not played: {cue}", flush=True)
        except MpvError as e:
            print(f"cue {cue} failed: {e}", flush=True)

    def on_serial_event(event_name):
        event_name = event_name.strip()
        if not event_name:
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

    try:
        if state["audio_loop"]:
            audio.play(state["audio_loop"], loop=True)
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
        serial.stop()
        shutdown()


if __name__ == "__main__":
    main()
