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
| `components` | Which colour drives each major component            |
| `default_decay_rate` | How fast a component's charge drains, % per second (optional) |
| `charge_per_pull` | How much one pull adds to a component's charge, % (optional) |
| `ambient_sounds` | Sounds overlord repeats on a schedule (optional)    |
| `audio_gain` | Loudness multiplier per audio cue (optional)        |
| `players` | Satellite Pis a cue may be addressed to (optional)  |
| `devices` | Devices the game sets and hears from: lights, latches, switches (optional) |
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
to fit one packet: 1 to 63 characters of lowercase letters, digits, `_`, `-` and
`*`. A `*` is a wildcard the player resolves when the cue arrives: the cue plays
one of the media files whose name matches, chosen at random and never the same
one twice in a row. `video_once_janis_nag_*` plays any of the `janis_nag_` clips.
The list is checked, not numbered: a state may only fire cues listed here, and
the order does not matter.

The file in `overlord/media/<folder>/` is named after the cue (`audio_once_chime.mp3`;
the name without its prefix, `chime.mp3`, also works). A future script may validate this
list against `overlord/media/` and build it into code.

Style: write lists and maps in multiline block form, not `{...}` / `[...]`.

## Players

```yaml
players:
  - shrine
  - grove
```

The Pis that play cues besides the Overlord (see `overlord/README.md`,
"Satellite players"). A cue is addressed to one by a prefix and two
underscores, and is played by that Pi alone:

```yaml
cues:
  audio_once:
    - audio_once_chime             # the Overlord
    - grove__audio_once_bell       # the grove Pi
  image:
    - shrine__image_show_cage      # a still on the shrine Pi
    - shrine__image_off
```

A cue with no prefix is the Overlord's (`overlord__` also works). The name before
`__` must be listed here, and what follows must be something a player plays:
`audio_once_<name>`, `audio_loop_<name>`, `audio_loop_off`, `image_show_<name>` or
`image_off` (the last two only on an `image` player). The whole name still has
to fit the 63-character limit above. Only the Pis read this list.

## Components

```yaml
components:
  major:                         # major components 1-5, in order: game1 colours
    - red
    - blue
    - green
    - yellow
    - orange
```

Which colour drives each major component. The position in the list is the
component's number, so the first entry is major component 1, and that order is
also the order of the bars on the component state display. There are always
exactly five.

A major is a colour: one of game1's five (`red`, `blue`, `green`, `yellow`,
`orange`), each listed once. A pull on any rope whose channel wears that colour
today drives it, so several ropes can feed one component, and recolouring a
channel on the arbiter's COLOURS screen changes which component its ropes drive
from the next pull. Purple is game2's colour and is not a major.

The finals are not configured here: every channel painted purple on the
COLOURS screen is a final of its own, so painting a channel purple is what
makes it one. Game2 is won (`all_final_components_activated`) when every purple
channel that has a rope mapped to it is lit at once; a purple channel with no
rope is left out, since nobody could pull it. A `final` list here is an error.
The component state display has bars for the first three roped purple
channels; more than three still count toward the win, without a bar.

The arbiter reads this, from its built-in game or a posted one; a new mapping,
or a recoloured channel, applies to the next pull. Overlord does not use it: the keyboard
simulator's keys stand in for the components themselves.

## Component decay

```yaml
default_decay_rate: 6.6667       # percent of full charge lost per second
charge_per_pull: 20              # percent of full charge one pull adds
```

A component's charge is a number from 0 to 100. Every pull on a rope driving it
adds `charge_per_pull`, and it drains at `default_decay_rate` the whole time, so
filling one takes pulls coming faster than the drain -- no one pull does it.
When the charge reaches 100 the component counts as activated, and it stays
activated until it has drained all the way to 0, when it counts as deactivated;
pulls in between top it up. Left alone from full it stays lit `100 / rate`
seconds: 6.6667 is 15 seconds, 10 is 10, 5 is 20. Both apply to every component,
majors and finals alike. `charge_per_pull` must be above 0 and at most 100 (100
is the old rule: one pull fills it); if left out it is 20.

The arbiter owns both: it decides when a component lights and goes dark, and
publishes each charge and the drain rate to the displays, so a display never has
its own copy. The rate must be above 0 and at most 65; if left out, the default
is the 15-second rate. `overlord`'s keyboard simulator reads the same values,
one key press standing in for one pull. A game posted to the arbiter takes
effect at once, from the charge each component has.

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

## Devices

```yaml
devices:
  diorama:
    set:
      lights: [dark, creepy]       # the first value is where it starts
      latch: [locked, open]
    emits: [door_opened, door_closed]
  party:
    set:
      lights: [dark, lit]
```

Things in the room the game drives directly, rather than through media: the
diorama's lights and door latch, the party lights. Each device lists the
settings it takes and the values each may hold, and the inputs it can send
back.

A state changes a setting with `set:` (below), and the setting **holds** until
another `set:` changes it -- unlike a cue, which happens once. The arbiter keeps
the whole table and broadcasts it on every change and once a second, so a
device that reboots or misses a packet is right again within a second.

A device that hears nothing from the arbiter for 12 seconds falls back to its
own safe state -- lights out, door locked -- so losing the arbiter never leaves
the room stuck. That fallback lives in the device's firmware, not here.

An input arrives as an event named `device.input`, which a state reacts to like
any other: `on: diorama.door_opened:`.

Names are lowercase words, and so are values. YAML reads a bare `on`, `off`,
`yes` or `no` as true/false, so pick other words (`lit`/`dark`) or quote them.
Names cross the air as short hashes; the validator refuses two that collide,
and a setting with values that collide.

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
    idle:                        # optional; fires while nothing is pulled
      delay: 20s
      cue:
        - some_cue
    at:                          # optional; timed steps, each once
      10s:
        set: { diorama.lights: creepy }
        cue:
          - some_cue
```

Anywhere `cue:` goes -- `on: enter:`, an event, `idle`, an `at` step -- a
`set:` may go too, with device settings to change:

```yaml
    on:
      enter:
        set:
          party.lights: lit
          diorama.latch: locked
```

The settings change first, then the cues fire, so a clip starts in a room that
has already changed. Setting a value a setting already has does nothing.

`at` runs each step once, its delay after the state is entered; leaving the
state cancels the steps still to come, and re-entering starts them again. A
step has `set` and/or `cue` and no `goto` -- `after` is for leaving.

Anywhere a list of cues goes, an entry may be `choose_one:` with cues under
it; one of those is picked at random each time the list fires:

```yaml
        cue:
          - audio_once_chime
          - choose_one:
              - video_once_janis_warn_*
              - video_once_janis_beg_*
```

`idle` fires its cues once `delay` has passed with no event, then again every
`delay` until an event arrives. Any event the arbiter delivers re-arms it,
whether or not the state lists that event. Entering the state arms it, and
leaving cancels it. `after` runs alongside and is unaffected.

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
- `game_reset_components` clears the activation status of all major
  components and every final (purple) channel and immediately publishes zero charge to the displays. It emits no
  component-deactivation events. Put it before the round's goal-randomization
  cue when a fresh round should start with every component dark.
- A device's input is an event too, named `device.input` (see Devices), and
  only exists if the device lists it under `emits`.
- An event with no entry in the current state is ignored.
- `after` is cancelled when the state is left and restarts on re-entry.
  Durations are whole seconds or minutes: `10s`, `2m`. Seconds are the finest
  granularity; there is no `ms`. `2m` is the default delay.
- Order on a transition: the event's `cue`, then the new state's `on_enter_cue`.
- A `video_once_*` clip fired on entering a state cuts off any clip still
  playing; one fired for an event is dropped if a clip is playing. The cue
  carries which it was over the mesh, so the player needs no state table.
