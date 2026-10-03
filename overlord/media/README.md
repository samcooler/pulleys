# Media

The folder a file is in and its name decide when it plays. The file name
(without extension) is the event name: `sounds/door.mp3` plays on `sound_door`.
Keep names lowercase with no spaces.

## `video_loops/`

Full-screen background video that loops forever. `idle.*` plays at startup and
whenever no clip is running, so always include one. Switched with
`video_loop_<name>`.

## `audio_loops/`

Background music or ambience that loops forever, independent of the video. Use
a file whose end joins its start. `idle.*` plays at startup. Switched with
`audio_loop_<name>`, stopped with `audio_loop_off`.

## `clips/`

Short videos (with their audio) that play once on screen, then return to the
video loop. Triggered with `clip_<name>`.

## `sounds/`

Short sound effects played once on top of everything. They can overlap each
other. Triggered with `sound_<name>`.

---

Video: H.264 `.mp4` at the panel resolution. Audio: `.mp3`, `.wav` or `.ogg`.

The files committed here are test media. On the Pi, the real media is synced
from the rclone remote into `/var/lib/overlord/media` on every boot, so the
remote needs these same four folders at its root.
