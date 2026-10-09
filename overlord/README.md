# Overlord

Fullscreen video and audio panel driven by events from an ESP32. It plays a
looping background video and a looping background audio track. Trigger events
play a video clip once (then return to the video loop) or overlay a sound effect
on top of everything.

```
ESP-NOW commands -> bridge node -> serial (115200, one event per line) -> overlord.py -> 3 kinds of mpv
                                                                                         video player (fullscreen)
                                                                                         audio loop player (headless)
                                                                                         one process per sound effect
```

## Install

macOS:

```sh
brew install mpv ffmpeg      # ffmpeg is only needed to cut clips
pip install pyserial         # only needed for real serial; --test mode works without it
```

Python 3.10+.

## Run

From this directory:

```sh
# Dev / test: window instead of fullscreen, events come from a Unix socket
python3 overlord.py --test --windowed

# Real hardware: fullscreen, events come from the ESP32 over serial (auto-detected)
python3 overlord.py

# Real hardware, explicit port
python3 overlord.py --serial-port /dev/cu.usbmodemXXXX
```

Ctrl-C quits. Extra mpv flags can be passed with `--mpv-arg`, repeatable. They
go to every player (video, audio loop and sound effects):

```sh
python3 overlord.py --test --windowed --mpv-arg=--mute=yes
```

On start you should see `starting audio loop: idle.wav` and
`starting idle loop: idle.mp4` (each only if the file exists).

## Deploying on a Raspberry Pi

Targets Raspberry Pi OS Lite (Bookworm). On the Pi, clone the repo and run:

```sh
sudo overlord/deploy/install.sh
```

This installs mpv/rclone/plymouth and sets up:

- **Splash:** a plymouth theme showing `deploy/splash.png` (replace it with your
  art, then re-run `install.sh`) with the console quieted. The splash stays up
  until mpv takes over the screen.
- **Kiosk:** `overlord.service` starts at boot and restarts on crash. mpv draws
  straight to the display over DRM/KMS, so no desktop is needed. The login
  prompt on tty1 is masked; ssh still works.
- **Boot update:** `overlord-update.service` runs before the app on every boot:
  `git pull --ff-only`, then `rclone sync` of `OVERLORD_MEDIA_REMOTE` into
  `/var/lib/overlord/media` (the app reads it via `OVERLORD_MEDIA`). Both are
  best-effort: with no network the panel starts on the code and media it has.
  The sync mirrors the remote, so files deleted there are deleted on the Pi.

Config lives in `/etc/overlord.env` (media remote, serial port, mpv audio
device). Logs: `journalctl -u overlord-update -u overlord -b`.

## Trigger events

With overlord running in `--test` mode, from another terminal:

```sh
./send.sh video_once_test1          # play media/video_once/test1.* once on the video
./send.sh audio_once_beep           # overlay media/audio_once/beep.* (overlaps with everything)
./send.sh audio_loop_idle           # switch the background audio loop (generated test tone)
./send.sh audio_loop_ambient        # switch to the ambient music loop
./send.sh audio_once_steam          # overlay the steam hiss
./send.sh audio_loop_off            # stop the background audio loop
./send.sh video_loop_idle           # switch the background video loop
./send.sh audio_once_beep audio_once_chime    # several events in one call
```

`send.sh` writes to `/tmp/overlord.sock` and exits immediately. Override the
path with `OVERLORD_SOCK=/path ./send.sh ...` (set the same variable when
starting `overlord.py --test`; handy for a second instance).

Expected overlord output for a clip:

```
event: video_once_test1
mpv: file-loaded
mpv: end-file eof
clip finished, returning to loop
```

An unrecognised event prints `unknown event: <name>` and changes nothing. A
recognised prefix with no matching file prints
`no media for audio_once_nope in media/audio_once/`.

On real hardware the same event names are sent as newline-terminated text over
serial. To fake it without the bridge node, write a line to the port, e.g.
`echo video_once_test1 > /dev/cu.usbmodemXXXX`.

### Events

Events are matched by prefix and then by file name, so adding media needs no
code change: drop `media/audio_once/door.mp3` in and `audio_once_door` works. The
extension is ignored (first match by name wins), and names can't contain `/`.

| Event                | Action                                                                          |
| -------------------- | ------------------------------------------------------------------------------- |
| `video_once_<name>`  | Play `media/video_once/<name>.*` once on the video, then back to the video loop |
| `audio_once_<name>`  | Play `media/audio_once/<name>.*` once, overlaid; sounds overlap freely          |
| `video_loop_<name>`  | Switch the background video to `media/video_loops/<name>.*`                     |
| `audio_loop_<name>`  | Switch the background audio to `media/audio_loops/<name>.*`                     |
| `audio_loop_off`     | Stop the background audio loop                                                  |

Behaviour details:

- A `video_loop_` sent while a clip is playing takes effect when the clip ends.
- Triggering a clip while another is playing replaces it.
- The audio loop and sounds are independent of the video: clips and video loop
  switches never interrupt them. A clip's own audio track plays through the
  video player, on top of the audio loop. Sounds are not ducked.
- Loops start from `idle.*` in `video_loops/` and `audio_loops/` if present. If
  several `idle.*` files exist the first by name wins (`idle.mp3` beats
  `idle.wav`). To start on the ambient track, rename it to `idle.mp3` and remove
  `idle.wav`.

## Media

```
media/video_loops/idle.mp4   background video loop, played at start and after every clip
media/audio_loops/idle.wav   background audio loop, played at start (generated test tone)
media/audio_loops/ambient.mp3  56 s ambient music loop (audio_loop_ambient)
media/video_once/*.mp4       one-shot video clips (video_once_<name>)
media/audio_once/*.wav       one-shot overlay sounds (audio_once_<name>)
media/audio_once/steam.mp3   1.1 s steam hiss (audio_once_steam)
test_full.mp4                1080p 60s source used to cut test1
test2_full.mp4               4K 20s source used to cut test2
```

`audio_loops/idle.wav`, `audio_once/beep.wav` and `audio_once/chime.wav` are generated
test tones. `audio_loops/ambient.mp3` and `audio_once/steam.mp3` are real
third-party audio files (originally `artifystudio-looping-ambient-7-203914.mp3`
and `dragon-studio-steam-hissing-386157.mp3`); check their licences before
shipping. Levels are untouched, so the steam hiss may sit louder or quieter than
the ambient loop; adjust with ffmpeg `-af volume=...` if needed. Regenerate the
test tones with:

```sh
ffmpeg -y -f lavfi -i "sine=frequency=220:duration=4" -af "tremolo=f=2:d=0.6,volume=0.4" media/audio_loops/idle.wav
ffmpeg -y -f lavfi -i "sine=frequency=880:duration=0.4" -af "afade=t=out:st=0.2:d=0.2" media/audio_once/beep.wav
ffmpeg -y -f lavfi -i "sine=frequency=520:duration=1.2" -af "afade=t=out:st=0.6:d=0.6" media/audio_once/chime.wav
```

For a seamless audio loop use a file whose end joins its start; `loop-file=inf`
restarts the file with no crossfade.

Re-cut the test clips (3 s from the start of each source). This ffmpeg build has
no `libx264`, so use the VideoToolbox encoder:

```sh
ffmpeg -y -i test_full.mp4  -t 3 -c:v h264_videotoolbox -b:v 20M -pix_fmt yuv420p -c:a aac media/video_once/test1.mp4
ffmpeg -y -i test2_full.mp4 -t 3 -c:v h264_videotoolbox -b:v 40M -pix_fmt yuv420p -c:a aac media/video_once/test2.mp4
```

The folders were renamed from `media/loops` to `media/video_loops`; if you have
an older checkout, move it.

The one-shot folders and events were renamed too: `media/clips` is now
`media/video_once` (`clip_<name>` is `video_once_<name>`) and `media/sounds` is
now `media/audio_once` (`sound_<name>` is `audio_once_<name>`). The rclone
remote needs the same folder names.

## Files

- `overlord.py` - entry point; prefix-based event dispatch, loop/clip state machine.
- `mpv_ipc.py` - `Mpv` drives one mpv over its JSON IPC socket (fullscreen kiosk, or headless audio with `kiosk=False`); `SoundPlayer` spawns one throwaway mpv per sound effect.
- `serial_reader.py` - line-based event source: serial port (auto-reconnects) or, with `--test`, the Unix socket.
- `send.sh` - sends trigger events to the test socket.

## Design notes / gotchas

- **mpv events are handled on a worker thread.** `Mpv._read_loop` only reads the
  socket and queues events; `_event_loop` runs `on_event`. Handlers call
  `mpv.play()`, which blocks waiting for a reply that only the reader thread can
  deliver. Running handlers on the reader thread deadlocked and raised
  `MpvError: timeout: ('set_property', 'loop-file', 'inf')`, so the clip never
  returned to the idle loop.
- **`--keep-open=no`.** With `yes`, mpv freezes on the last frame at the end of a
  clip and never sends `end-file`, so the return to the idle loop could not fire.
  The idle loop uses `loop-file=inf`, so it never ends.
- **A clip counts as finished only on `end-file` with `reason == "eof"`.** The
  `stop` reason also fires when a clip replaces the idle loop and would cut the
  clip short.
- **Three kinds of player.** The video mpv owns the window. The audio loop is a
  separate headless mpv (`--no-video`) so loops and clips never touch each
  other. Each sound effect is its own short-lived mpv process, which is how they
  overlap; there is no mixer, the OS mixes. Startup is a few hundred ms per
  sound, so expect a small trigger latency.
- **Shared state is locked.** The serial thread and the mpv event thread both
  touch the loop/clip state, guarded by one lock in `overlord.py`.
- **`send.sh` uses Python, not `nc`.** The server reads a connection until the
  client closes it. macOS `nc -U` does not close after stdin EOF, so it hangs.
  Python closes the socket right after sending.
- Only one test client is accepted at a time (`listen(1)`); `send.sh` is short-lived so this is not a problem in practice.
