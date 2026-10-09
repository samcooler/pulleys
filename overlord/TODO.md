# TODO

## 1. Test the deploy on a real Raspberry Pi

`deploy/install.sh` has never been run. Use a spare SD card (Pi OS Lite, Bookworm).

- [ ] Clone the repo on the Pi, run `sudo overlord/deploy/install.sh`, reboot
- [ ] Splash shows on boot (no rainbow, no console text, no cursor)
- [ ] Splash hands off to mpv without flashing the console
- [ ] Idle video and audio loop start on their own
- [ ] Git access works without a password (deploy key or HTTPS remote)
- [ ] Set up the rclone remote (`rclone config`), set `OVERLORD_MEDIA_REMOTE`, confirm media syncs at boot
- [ ] Unplug the network, reboot: panel still starts on existing code and media
- [ ] Push a code change, reboot: the Pi picks it up
- [ ] Serial: bridge arrives as USB (`/dev/ttyACM*`) or GPIO UART; set `OVERLORD_ARGS` if needed
- [ ] HDMI audio: pick the right `--audio-device` if there's no sound
- [ ] Video performance: check 1080p clips play smoothly with hardware decode
- [ ] Pull the power a few times: it comes back up unattended
- [ ] Replace the placeholder `deploy/splash.png`

## 2. Repeating ambient noises

Sounds that play periodically on a schedule, started and stopped by serial events.

- [ ] Decide the event names (e.g. `ambient_start_<name>`, `ambient_stop_<name>`, `ambient_stop_all`)
- [ ] Decide where sounds and schedules live (new `media/ambient/` folder? interval config per file or in the event?)
- [ ] Decide the schedule shape: fixed interval vs. random range (e.g. every 20-60 s)
- [ ] Implement a scheduler thread in `overlord.py`; several ambient sounds can run at once
- [ ] Stopping must cancel the pending timer; starting an already-running one shouldn't double it
- [ ] Clean shutdown (cancel all timers in `shutdown`)
- [ ] Add to `send.sh` examples, top-level `README.md` and `media/README.md`

## 3. Test with a real ESP32 over serial

So far only tested with `--test` mode and the Unix socket.

- [ ] Plug in the bridge node, confirm overlord auto-detects the port and prints `serial: connected`
- [ ] Trigger an event on the ESP-NOW network and confirm overlord receives and plays it
- [ ] Check the line format the bridge sends matches what overlord expects (bare `video_once_test1`, not `EV ...`; see ARCHITECTURE.md bridge section)
- [ ] Unplug and replug the bridge: reader reconnects
- [ ] Reboot the Pi with the bridge attached: events work after boot
- [ ] Measure latency from sensor trigger to playback
