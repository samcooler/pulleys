# Game format

A game is one YAML file in this folder: a state machine. Events come in, states
react by firing cues or moving to another state. A cue is one of the trigger
strings overlord already accepts (`clip_test1`, `sound_chime`,
`audio_loop_idle`, ...). See `example.yaml`.

## Top level

| Key      | Meaning                                              |
| -------- | ---------------------------------------------------- |
| `game`   | Name, for logs                                       |
| `start`  | State to enter at launch                             |
| `cues`   | Every cue the game may use, grouped by type, sorted  |
| `states` | The state machine                                    |

```yaml
cues:
  audio_loop:
    - audio_loop_idle
    - audio_loop_off
  clip:
    - clip_test1
  sound:
    - sound_chime
  video_loop:
    - video_loop_idle
```

The file name in `overlord/media/` is the cue suffix. A future script may validate this
list against `overlord/media/` and build it into code.

Style: write lists and maps in multiline block form, not `{...}` / `[...]`.

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
      delay: 2m
      goto: other_state
```

- Events used so far: `component_activated` and `component_deactivated` (any
  major component), `all_components_activated` and `all_components_deactivated`. The event source decides when they fire.
- An event with no entry in the current state is ignored.
- `after` is cancelled when the state is left and restarts on re-entry.
  Durations are `500ms`, `10s`, `2m`.
- Order on a transition: the event's `cue`, then the new state's `on_enter_cue`.
