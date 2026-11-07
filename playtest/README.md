# Playtest

Game design for the ropes, with simulated players before the event and real
ones during it.

The installation is a new kind of controller: twenty-odd ropes in six colours,
pulled by whoever walks up, answered by a video, sounds, lights and a door.
Nobody knows yet which games are fun on it. This folder is how to find out,
in four parts that share one engine and one log format:

| Part | What it is | File |
| ---- | ---------- | ---- |
| Engine | The game, on a clock you hand it. A copy of the arbiter's C++; overlord's `--game` runs it too. | `engine.py` |
| Simulated visitors | People who walk up in groups, notice some of what happens, learn which colours are wanted, and leave when bored. | `players.py`, `world.py` |
| Fun score | One number per session, from what the room can observe. | `metrics.py` |
| Tuning | Offline: search the game's knobs with simulated visitors. Live: learn from real visitors which knob settings they enjoy, session by session. | `tuner.py`, `live.py` |

The game's knobs are its **params** (`game_state_machine/GAME_FORMAT.md`):
named values with a range, which the arbiter accepts live. The designer
decides which knobs exist and how far they go; the tools only turn them.

## Quick start

From the repo root, with `pyyaml` installed:

```sh
python3 -m playtest sim                                   # the installation's game, 200 simulated visits
python3 -m playtest sim game_state_machine/games/chase.yaml
python3 -m playtest compare \
    game_state_machine/state_config.yaml \
    game_state_machine/state_config.yaml:marks=0,show_goals=0 \
    game_state_machine/state_config.yaml:charge_per_pull=30,round_time=3m
python3 -m playtest tune --out tune.json                 # search the knobs (a few minutes)
python3 -m playtest validate                              # does the fun score track simulated enjoyment?
python3 -m playtest live-sim --hours 24 --prior tune.json # the live director over a simulated day
python3 -m playtest analyse /var/lib/overlord/playlog/play-*.jsonl   # real sessions, scored
python3 -m playtest calibrate play-*.jsonl --out population.json     # fit the simulated crowd to real ones
python3 -m unittest discover -s playtest/tests -t .       # both engines' behaviours (C++ needs pio run -e arbiter_mesh once)
```

A variant is a game file with param values or options after a colon:
`state_config.yaml:round_time=90s,show_goals=1`. `marks=0` takes away the
extractors' goal outlines, to see the room without them. `--population
population.json` runs any command with a calibrated crowd.

## Designing a game

1. **Write it.** A game is a YAML state machine. Besides states, cues and
   timers, the format has the parts these games need: per-pull events
   (`goal_pull`), counters with a mark (`score_reached`), a repeating timer
   (`every`), colour shuffles (`colors: shuffle`), goals dealt by count
   (`goals: $easy_goals`), and `show_goals` to put the wanted colours on the
   screen. `game_state_machine/games/` has three worked examples:

   - `bop_it.yaml`: a colour is called; pull it before the window closes.
     Streaks speed it up, misses end it.
   - `chase.yaml`: one colour is hunted and hops every few seconds; every pull
     on it scores.
   - `shifting_ropes.yaml`: Simon, but the ropes swap colours every 30 s.

2. **Check it.** `python3 tools/game_to_json.py <file>` refuses a game that
   names a missing state, cue, counter or param, or puts a duration param
   where a count goes.

3. **Play it in simulation.** `sim` plays 200 simulated visits and reports
   how long people stayed, how far they got, why they left (by kind of
   visitor), the fun score, and the simulator's own ground-truth enjoyment.
   `compare` runs the same visitors through several variants, so differences
   are the game's and not the dice's.

4. **Name its knobs.** Every number you are unsure of becomes a param with a
   range. `tune` sweeps each one, then climbs to the best combination, and
   re-checks the winner on fresh visitors (the climb flatters itself on the
   ones it chose with). The sweep's spread per param says which knobs matter.

5. **Play it by hand.** `python3 overlord/overlord.py --test --windowed --game
   <file> --simulate` runs it on overlord's own engine, keys 1-8 as ropes.

6. **Run it in the room.** Post it to the arbiter
   (`tools/game_to_json.py --post 192.168.4.1`) or build it in. Overlord keeps
   the play log; `analyse` scores the real sessions; `calibrate` fits the
   simulated crowd to them, so the next round of simulation is about the
   people who actually come.

7. **Let it learn.** With `OVERLORD_DIRECTOR=1` overlord runs the live
   director, seeded with `tune.json`'s prior: between sessions it sets the
   knobs, after each it scores what happened, and over the event it settles
   on what real visitors enjoy. `curl localhost:8080/playtest` shows what it
   believes; `/playtest/freeze` stops it.

## The simulated visitors

A visitor stands at a rope and pulls it. They know only what the room shows:
the rope colours, their own colour's bar on the component display, the clips
and sounds, the lights. They do not see the game's state, or its goals unless
`show_goals` draws them. In a Simon round they learn which colours are wanted
the way a person would: Janis praising an activation teaches "that colour",
silence after one teaches "not that". A clip that overlord drops because
another is playing teaches nothing.

Each visitor has three numbers over time:

- **affect**: how good the moment feels. Praise, progress, celebrations,
  reveals and novelty raise it; scolding, losses and nagging lower it. It
  fades back to zero over about ten seconds. Its integral is the visitor's
  **true fun**, the ground truth only the simulator has.
- **interest**: whether they stay. Affect feeds it; boredom wears it down,
  faster the longer nothing new has happened. They leave at zero, when their
  time runs out, or with their group.
- **beliefs**: per colour, how sure they are that pulling it is wanted.

When the diorama unlatches during the party, visitors may leave their ropes
to look inside. The curious and attentive are more likely to go. Some read the
signage and learn that purple frees the spirits, and they tell their group.
Some press "free the spirits" once or several times; its purple flash is a
hint an attentive presser may take. Once a visitor knows the twist, Janis
panicking at purple stops being a reason to stop.

Their pulls go through a model of the rope's sensor: it counts one pull,
then ignores the rope for 1.7 s, so yanking faster gains nothing. Some yanks
are rough. A rough yank trips the sensor's guard, so nothing counts and the
handle stays dark until the rope has been calm for 2.5 s, and an attentive
visitor who notices eases off. Party crowds and rebels yank roughest.

Five kinds of visitor come in groups: passers-by, families, solvers, rebels
(who pull purple because Janis says not to), and party crowds. Each kind has
its own pull rate, attention, curiosity, compliance, patience and time
budget, scattered per person.

**Every number in the visitor model is a guess.** It was written to behave
plausibly, not fitted to anyone. Treat simulation results as hypotheses about
the game, worth checking in the room. `calibrate` exists to replace the
guesses once there are real logs (see Limits).

## The fun score

Nobody can measure fun from a pulley. What the room can observe is revealed
engagement, so the score per session is:

| Term | Weight | Measures |
| ---- | ------ | -------- |
| dwell | 1.0 | log2(1 + minutes of continuous play) |
| progress | 1.0 | how far through the game it got, 0-1 |
| advances | 0.6 | rounds won, up to 4, scaled 0-1 |
| setback | -0.6 | walked off within 15 s of a round timing out |

A session is a run of pulls with no gap over 40 s. In the simulator, across
varied settings, this score correlates with simulated true fun at r = 0.80
with goals hidden and r = 0.92 with them shown (`validate`). A "sustain" term
for late against early pull rate was dropped: the best sessions end in a party
that needs no pulling, so it pointed the wrong way.

## The live director

On the Pi, inside overlord, the director watches the play log. When nobody has
pulled for 40 s it scores the session that just ended, credits the score to
the knob settings that were live, and picks the next session's settings by
Thompson sampling: each knob's levels keep an estimate of the fun they led to,
it draws a plausible value for each level and takes the best draw. So it
mostly plays what has worked and keeps trying what might still be better.
Old sessions count for a little less each time (a factor of 0.995 a session),
so it follows a crowd that changes over days.

- Give it one or two knobs (`OVERLORD_DIRECTOR_FOCUS=charge_per_pull`). With
  a prior from `tune` and no focus, it only turns the knobs whose levels made
  a real difference in simulation (`min_spread`). A few hundred sessions are
  not enough to learn nine knobs at once.
- The prior is used as how much better each level is than the others, not as
  absolute scores. A simulated single visit and real overlapping play score on
  different scales; which level beats which should carry over.
- It changes settings only between sessions, never under someone's hands. It
  can never go outside the game file's ranges: the arbiter clamps them.
- Its state survives restarts (`director.json` beside the logs).

## What simulation says about the installation's game

These come from the simulated visitors, so they are hypotheses, except where a
finding is about how the game or the hardware is built.

1. **The game could get stuck for good (fixed).** A hard round dealt all five
   colours as goals, and nothing clears goals. Back in `passive`, every
   activation counted as a goal activation, and `passive` only listened for
   non-goal ones. After any session that reached the hard round, nothing could
   start the game again until the arbiter rebooted. `passive` now starts on
   either kind; a test guards it.

2. **Nothing used to show the goals (fixed).** The arbiter never told any
   display which colours a round wanted, so the only clue was Janis praising
   some activations and ignoring others. Now it sends them with the charges
   while a round is on, each extractor outlines its tank (top and bottom rows
   of the ring, half brightness) while its colour is wanted, and overlord draws
   them over the video (`show_goals`).

3. **A rope counts a pull at most every 1.7 s.** That is the sensor's pull
   plus its 1.5 s cooldown (`pulleys_detect`). At the old 20% a pull, one rope
   added about 12% a second against a drain of about 7%: some 20 seconds of
   steady pulling per colour. The game file's note assumed two pulls a second
   from one rope. `charge_per_pull` is now 40, so three counted pulls fill a
   colour.

4. **Small groups could not finish.** With five goals in the hard round, one
   or two people could never hold them all lit. Goal counts are now 1, 2 and 3.

   Simulated visits that reach the party / free the spirits:

   | | Groups of 1-2 | Groups of 3+ |
   | --- | --- | --- |
   | Before: 2/3/5 goals, 20% a pull, no outlines | 0% / 0% | 0% / 0% |
   | New values, no outlines | 0% / 0% | 0% / 0% |
   | New values, outlines | 25% / 7% | 99% / 80% |

   Neither change works without the other.

5. **The final depends on the diorama.** The rounds punish purple, then the
   final asks for it. What turns people around is the diorama: it unlatches
   during the party, and inside are the spirits having their souls extracted,
   signage hinting that purple frees them, and a "free the spirits" button
   that flashes purple and is refused. Without the diorama in the model, 1%
   of simulated visits won the final; with it, most groups that reach the
   final win it. That rests on guesses about how many people look inside and
   read the signage. The play log records `diorama.door_opened` and
   `diorama.button_pressed`, so real nights will show it.

6. **Knobs matter less than structure.** Tuning settings gains a few tenths
   of the fun score; showing the goals and fitting the pace to the sensors
   decided whether the game worked at all.

## Limits

- **Visitor model.** Its numbers are guesses. Calibration matches what a log
  can show (session length, pull rate, ropes in use, progress, purple share),
  but different crowds can produce the same session statistics. Tested on a
  simulated "real" log from a known crowd, the fit matched the statistics
  closely while getting the crowd's pull rate and arrival rate wrong. Treat a
  calibrated crowd as one that produces realistic sessions, not as a
  measurement of who came.
- **Fun score.** It is a proxy, checked only against the simulator's own
  notion of fun. Whether it tracks real enjoyment needs watching people play.
  The weights are in `metrics.FUN_WEIGHTS`.
- **Live director.** Session scores are noisy (a standard deviation of about
  1.3 on a mean near 3), mostly from who walks in, and the knob effects are a
  few tenths. In simulation, over 72 hours at 20 groups an hour (about 470
  sessions), it did not beat the defaults:

  | Run | Defaults | Director |
  | --- | --- | --- |
  | all nine knobs | 2.78 | 2.75 |
  | all nine, with a prior | 2.78 | 2.76 |
  | goals shown, with a prior | 4.00 | 3.83 |
  | `charge_per_pull` only, three seeds | 2.78, 2.74, 2.85 | 2.69, 2.85, 2.83 |

  On the single knob it did find the right answer every time: its best
  estimate was 40 or 50, where fixed-setting runs put the gain at about +0.3.
  What it has not done in three days is earn back its exploring. So read it
  as an instrument: point it at one knob, let it run for days, and read its
  estimates (`GET /playtest`) before committing a value to the game file.
  Adjusting scores for crowd size does not rescue it: ropes in use in a
  session's first 30 s explain little of the score, and total ropes used is
  an outcome the knob itself changes. Structural changes (showing goals) are
  worth far more than tuning, and only a designer makes those.
- **Two engines.** The arbiter's C++ and `engine.py` implement the same
  rules separately. `tests/test_conformance.py` builds the C++ for this
  computer (`host/`) and plays the same scenarios through both, including the
  installation's whole game, start to finish and round again. The firmware
  builds for every live board, but the new parts have not run on one yet:
  the telemetry floods, the bridge's `GI` and `GAME-PARAM` lines, and the
  game-only colour shuffle on a real palette.
