# Media

The folder a file is in and its name decide when it plays. Name the file after
the whole cue: `audio_once/audio_once_door.mp3` plays on `audio_once_door`. The
short form without the prefix, `audio_once/door.mp3`, works too; if both exist
the full name wins. Keep names lowercase with no spaces.

## `video_loops/`

Full-screen background video that loops forever. `idle.*` plays at startup and
whenever no clip is running, so always include one. Switched with
`video_loop_<name>`.

## `audio_loops/`

Background music or ambience that loops forever, independent of the video. Use
a file whose end joins its start. `idle.*` plays at startup. Switched with
`audio_loop_<name>`, stopped with `audio_loop_off`.

## `video_once/`

Short videos (with their audio) that play once on screen, then return to the
video loop. Triggered with `video_once_<name>`. While a clip plays, another
`video_once` cue is dropped, unless it is one the game fires on entering a
state (or one sent by hand over the REST API): that one cuts the running clip off.

A `*` in the name plays one of the matching files at random, never the same
one twice in a row: `video_once_janis_nag_*` plays any of `janis_nag_thread.mp4`,
`janis_nag_essences.mp4`, ... Works in every folder.

## `audio_once/`

Short sound effects played once on top of everything. They can overlap each
other. Triggered with `audio_once_<name>`.

---

Video: H.264 `.mp4` at the panel resolution. Audio: `.mp3`, `.wav` or `.ogg`.

The files committed here are test media. On the Pi, the real media is synced
from the rclone remote into `/var/lib/overlord/media` on every boot, so the
remote needs these same four folders at its root.
