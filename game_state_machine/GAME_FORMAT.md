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
| `components` | Which rope channel drives each component            |
| `default_decay_rate` | How fast a lit component drains, % per second (optional) |
| `ambient_sounds` | Sounds overlord repeats on a schedule (optional)    |
| `audio_gain` | Loudness multiplier per audio cue (optional)        |
| `cues`   | Every cue the game may use, grouped by type                |
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

A cue goes over the mesh as its name in ASCII (`audio_once_chime`), so a name has
to fit one packet: 1 to 47 characters of lowercase letters, digits, `_` and `-`.
The list is checked, not numbered: a state may only fire cues listed here, and
the order does not matter.

The file in `overlord/media/<folder>/` is named after the cue (`audio_once_chime.mp3`;
the name without its prefix, `chime.mp3`, also works). A future script may validate this
list against `overlord/media/` and build it into code.

Style: write lists and maps in multiline block form, not `{...}` / `[...]`.

## Components

```yaml
components:
  major:                         # major components 1-5, in order
    - 1
    - 10
    - 3
    - 4
    - 5
  final:                         # final components 1-3
    - 6
    - 7
    - 8
```

Which rope channel drives each component. The position in a list is the
component's number, so the first `major` entry is major component 1, and that
order is also the order of the bars on the component state display. There are
always exactly five major and three final components, each on its own channel
(0-15); a channel listed twice is an error, and a channel not listed drives
nothing. The arbiter reads this, from its built-in game or a posted one; a new
mapping applies to the next pull. Overlord does not use it: the keyboard
simulator's keys stand in for the components themselves.

## Component decay

```yaml
default_decay_rate: 6.6667       # percent of full charge lost per second
```

A component's charge is a number from 0 to 100. A pull sets it to 100, and it
drains at this rate until it reaches 0, when the component counts as deactivated.
So it stays lit `100 / rate` seconds: 6.6667 is 15 seconds, 10 is 10, 5 is 20. It
applies to every component. The arbiter owns it: it decides when a component goes
dark and publishes the rate to the component state displays together with each
charge, so a display never has its own copy. It must be above 0 and at most 65.
If left out, the default is the 15-second rate. `overlord`'s keyboard simulator
reads the same value. A game posted to the arbiter takes effect from the next pull.

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

## Audio gain

```yaml
audio_gain:
  audio_once_chime: 1.2          # 20% louder
  audio_loop_idle: 0.8           # 80% as loud
```

Balances clips against each other. Each value multiplies that clip's own level,
on top of the volume setting; a cue not listed plays at 1. Keys are
`audio_once_*` and `audio_loop_*` cue names, values are 0 to 10. Above 1 can
clip a clip that is already loud. Overlord reads the file at startup, so a
change needs a restart; the arbiter ignores it.

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

- Events used so far: `first_goal_component_activated` (a goal component lit
  while no goal component was lit; it comes just before
  `goal_component_activated` for the same pull), `goal_component_activated`,
  and `nongoal_component_activated` (a major component outside the current goal
  set), `component_deactivated` (any major component), `all_components_activated`
  (all current goals lit and every non-goal major component dark) and
  `all_components_deactivated`,
  `final_component_activated`, `final_component_deactivated` and
  `all_final_components_activated`. The event source decides when they fire.
- `game_randomize_simon_N_goals` cues, for N from 1 to 5, choose N distinct
  major components as goals. A repeat of the prior set is avoided when another
  set of that size exists. Goals persist until another randomization cue.
- `game_reset_components` clears the activation status of all major and final
  components and immediately publishes zero charge to the displays. It emits no
  component-deactivation events. Put it before the round's goal-randomization
  cue when a fresh round should start with every component dark.
- An event with no entry in the current state is ignored.
- `after` is cancelled when the state is left and restarts on re-entry.
  Durations are whole seconds or minutes: `10s`, `2m`. Seconds are the finest
  granularity; there is no `ms`. `2m` is the default delay.
- Order on a transition: the event's `cue`, then the new state's `on_enter_cue`.
