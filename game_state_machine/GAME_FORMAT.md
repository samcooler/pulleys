# Game format

A game is one YAML file in this folder: a state machine. Events come in, states
react by firing cues or moving to another state. A cue is one of the trigger
strings overlord already accepts (`video_once_test1`, `audio_once_chime`,
`audio_loop_idle`, ...). See `state_config.yaml`.

## Top level

| Key      | Meaning                                              |
| -------- | ---------------------------------------------------- |
| `game`   | Name, for logs                                       |
| `start`  | State to enter at launch                             |
| `ambient_sounds` | Sounds overlord repeats on a schedule (optional)    |
| `cues`   | Every cue the game may use, grouped by type; order matters  |
| `states` | The state machine                                    |

```yaml
cues:
  audio_loop:
    - audio_loop_idle
    - audio_loop_off
  audio_once:
    - audio_once_chime
  video_loop:
    - video_loop_idle
  video_once:
    - video_once_test1
```

A cue goes over the mesh as two bytes, `[folder, file]`: the group's position
among the groups, then the cue's position within its group, both counted from 0
(`audio_once_chime` above is `[2, 1]` in the example config). So order is part of
the protocol: add groups and cues at the end, never reorder or delete, and keep
every count under 256.

The file in `overlord/media/<folder>/` is named after the cue (`audio_once_chime.mp3`;
the name without its prefix, `chime.mp3`, also works). A future script may validate this
list against `overlord/media/` and build it into code.

Style: write lists and maps in multiline block form, not `{...}` / `[...]`.

## Ambient sounds

Sounds that play on their own, whatever state the game is in: steam hissing
every few seconds, say. Only overlord reads this; `tools/game_to_json.py` leaves
it out of the arbiter's JSON.

```yaml
ambient_sounds:
  - sound: audio_once_steam
    interval: 5                  # seconds between plays
    variance: 1                  # plus or minus seconds, picked fresh each time
```

Each entry has its own timer and plays every `interval` seconds, give or take up
to `variance`, so the example plays every 4 to 6 seconds. The first play comes
one interval after overlord starts. `sound` must be an `audio_once_*` cue listed
under `cues`; `interval` must be above 0 and `variance` at least 0 and below
`interval`. Overlord reads the file at startup, so a change needs a restart.
Plays are not logged (the journal would fill up); one that fails is logged once.

## States

```yaml
states:
  name:
    on_enter_cue:                # fired when the state opens
      - some_cue
    on:
      event_name:
        cue:                     # fired when the event arrives
          - some_cue
        goto: other_state        # optional; move after the cues
    after:                       # optional one-shot timer
      delay: 2m                  # default
      goto: other_state
```

- Events used so far: `component_activated` and `component_deactivated` (any
  major component), `all_components_activated` and `all_components_deactivated`,
  `final_component_activated`, `final_component_deactivated` and
  `all_final_components_activated`. The event source decides when they fire.
- An event with no entry in the current state is ignored.
- `after` is cancelled when the state is left and restarts on re-entry.
  Durations are whole seconds or minutes: `10s`, `2m`. Seconds are the finest
  granularity; there is no `ms`. `2m` is the default delay.
- Order on a transition: the event's `cue`, then the new state's `on_enter_cue`.
