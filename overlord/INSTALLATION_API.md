# Installation controls

Open `http://overlord.local:8080/installation`. The Firmware page links to it.
This requires updated `arbiter_mesh` and bridge firmware (`bridge` or
`bridge_esp32`). Old sensors/screens still relay the new flood message types;
no sensor protocol change is required.

The arbiter owns the configuration and its storage. Overlord keeps only a live
readback cache. Choose an arbiter if more than one responds. Edits are disabled
when the bridge disconnects, the snapshot is older than 15 seconds, or firmware
work is active. The backend also enforces these transport limits. A request in
flight when transport disappears can have an unknown result: review refreshed
readback before repeating an action.

## Operator behavior

- **Map ropes:** Start opens an exclusive arbiter-owned session. The first pull
  opens a 5-second capture window for up to 10 distinct sensor IDs. After the
  window closes, Assign moves the whole group to a channel from 1 through 24.
  A new rope pulled after assignment starts the next group. Again clears the
  group; Done closes mapping. The touchscreen and browser share the same
  controller, and the touchscreen shows `MAP: REMOTE` while a browser owns it.
- While mapping is active, the arbiter consumes events before they reach its
  game, matching the touchscreen workflow. Radio telemetry and the play log
  still receive them. A visible browser renews its lease about every 6 seconds;
  the arbiter closes it after 30 seconds without renewal, including after a
  browser disappears, transport is lost, or firmware work blocks renewals.
  A local touchscreen session lasts until Done and cannot be taken over by a
  browser. Each new remote session has a new ownership token.
- Mapping rows distinguish assigned channels from actual reported channels.
  A report counts as current only while the node has been heard within 20
  seconds. A matching report is evidence of that sensor's channel, not proof
  that every device has adopted RGB or channel colours.
- **Colours:** choose any of the six named colours per channel. Shuffle permutes
  game1 colours while leaving purple and shared-colour groups intact.
- **RGB:** edit exact integer values and Apply. Held edits are not sent on each
  keystroke; the arbiter also waits 300 ms after operator changes settle before
  saving/broadcasting them. Reset restores all six factory RGB values.
- Operator map/palette/RGB edits persist on the arbiter. Storage is reported as
  `pending`, `saved`, or `failed`; a running change can fail to persist. Temporary
  game shuffles remain separate from the saved daily palette. An operator
  colour/RGB edit restores the daily palette first.
- **Clock:** manual entries are installation local wall time, with valid dates
  from 2024 through 2099. Set from Overlord uses the Overlord machine's local
  time, not the browser's timezone. The page displays that timezone and the
  arbiter's time/source. Cancel discards unfinished clock input. With no coin
  cell, clock recovery snapshots miss the outage duration; a failed recovery
  snapshot write is shown separately from setting the RTC itself.

## HTTP API

All endpoints share the existing control server and its LAN access policy.

| Method | Path | Body / result |
| --- | --- | --- |
| GET | `/api/installation` | Cached arbiter snapshots, freshness, bridge state, firmware-busy state, Overlord local time/timezone, last polling error |
| POST | `/api/installation/color` | Common fields plus `channel` (1–24), `color` (0–5) |
| POST | `/api/installation/rgb` | Common fields plus `color` (0–5), `r`, `g`, `b` (0–255) |
| POST | `/api/installation/goals` | Common fields plus `mask` (0–31): the goal colours as palette-colour bits, red=1, blue=2, green=4, yellow=8, orange=16. Purple is never a goal; a colour no major component wears is ignored. Needs arbiter firmware with `game.goal_set` |
| POST | `/api/installation/shuffle` | Common fields |
| POST | `/api/installation/reset_rgb` | Common fields |
| POST | `/api/installation/reset_game` | Common fields. Restarts the game at its start state; clears the trigger log, counters, component charge and pull history. Colours, map, game and clock are kept |
| POST | `/api/installation/map_begin` | Common fields plus `session` |
| POST | `/api/installation/map_again` | Common fields plus `session` |
| POST | `/api/installation/map_assign` | Common fields plus `session`, `channel` (1–24) |
| POST | `/api/installation/map_renew` | Common fields plus `session` |
| POST | `/api/installation/map_end` | Common fields plus `session` |
| POST | `/api/installation/clock` | Common fields plus `time`: `YYYYMMDDHHMMSS` |
| POST | `/api/installation/clock_now` | Common fields; Overlord samples its local time |

Common fields are `target` (four uppercase hexadecimal digits, nonzero), `boot`
(the arbiter's 16-digit uppercase hexadecimal boot identity), and `revision`
(the integer from the snapshot). Use a nonzero 16-digit uppercase hexadecimal
`session` token for mapping. It is bound to the originating bridge as well as
the token. This is exclusive workflow ownership, not authentication.

Mutation success returns `{"v":1,"ok":true,"installation":{...}}` after the
arbiter responds. The snapshot includes `id`, `boot`, `revision`, `storage`,
`channels` (active/daily colours and rope counts), `colors` (names/RGB), mapped
`sensors` (assigned/reported channels and last heard age), `game_shuffled`,
`mapping` (owner/kind/phase/countdown/lease/captured sensors), and `clock`
(presence/time/source/recovery-storage state). Cached GET views add `fresh` and
`age_seconds`. Polling observes touchscreen, serial, and game palette changes.

Invalid bodies return 400. Stale revisions, ownership conflicts, or busy work
return 409. Disconnected/stale/unavailable transport returns 503. Missing
readback returns 504 and must not be treated as successful application. Conflict
responses update Overlord's cached readback before returning an HTTP error.
Renew/end require the same boot and mapping owner but do not compare revision:
ordinary captures must not prevent lease renewal or closing the session.

## Lever debug

The Installation page ends with one button per rope channel (1-24). A click is
one pull on that channel, as if its sensor had fired: it charges the component,
lights goals, fires cues and reaches the play log. Clicks are independent, so
fast clicking pulls fast.

`POST /lever/<channel>` (1-31) on the control server does it: 200 `{"channel":n}`,
400 for a bad channel, 503 if the bridge is not connected. Overlord writes
`GAME-PULL <channel>` to the bridge, which floods `MESH_MSG_GAME_PULL` (`0xA6`,
body the channel in ASCII, two copies the mesh dedupes). The arbiter plays it
into the game through the same path as a sensor pull. It is not a sensor event,
so it is not written to the SD log, and it is dropped while the mapping screen
holds the ropes. With `--game` (no arbiter), Overlord's own engine takes the pull.
Needs bridge and arbiter firmware from this change.

## Bridge and mesh protocol, version 1

Overlord writes one line:

```
INSTALL {"v":1,"q":"0123456789ABCDEF","target":"ABCD","op":"get"}
```

The bridge forwards its JSON body as `MESH_MSG_INSTALL_REQ` (`0xA4`). Requests
fit in the mesh's 241-byte flood body. Only `get` can use target `0000`, for
arbiter discovery. Mutations carry the common fields and operation values above;
`clock_now` is converted by Overlord to `clock` before transmission.

The arbiter accepts requests from bridge origins, validates boot/revision and
values, and returns a compact JSON response split into chunks of at most 200
bytes. Each `MESH_MSG_INSTALL_RES` (`0xA5`) body is:

```
<q:16hex> <bridge:4hex> <part:02decimal> <count:02decimal> <raw JSON fragment>
```

Parts are zero-based; count is 1–64. The 28-byte ASCII header plus 200-byte
fragment fits in a flood body. Chunks are sent at most once per 100 ms, one
response at a time. The bridge accepts replies addressed to itself and emits:

```
IC <arbiter:4hex> <q:16hex> <bridge:4hex> <part:02decimal> <count:02decimal> <hex-encoded fragment>
```

Hex encoding protects spaces at fragment boundaries from serial line trimming.
The bridge has a separate bounded reply queue and transmits whole lines only
when UART/USB capacity is available; normal cue/event queues take priority.
Overlord bounds assembly to 64 chunks and eight responding arbiter/bridge pairs,
ignores wrong requests/targets, and supports duplicate/out-of-order chunks.

Overlord serializes configuration requests and retries the **same** request ID
and body every 7 seconds for up to 18 seconds. The arbiter retains its last eight
request/response pairs, keyed by bridge and request ID. Identical retries replay
readback without rerunning shuffle/reset/assignment. ID reuse with another body
is ignored. An evicted mutation with an old revision conflicts instead of
reapplying. Boot identities prevent pre-reboot edits from applying afterward.
Touch, serial, temporary game palette changes, mapping state transitions, and
clock edits advance the revision; clock ticking and lease renewal do not.

An arbiter busy transmitting another response leaves a new request unanswered;
Overlord retries within its bounded deadline. Snapshot caching is intentionally
transient, and protocol version/schema changes require compatible firmware.

## Validation and remaining hardware checks

`python3 -m unittest discover -s test` covers the installation HTTP API,
validation, chunk reassembly/retries/timeouts, stale/disconnected/busy states,
clock payloads, and a compiled C++ test of the shared capture/lease/calendar
logic. Existing firmware dashboard/CLI tests remain part of that run.

Build the affected firmware with:

```
pio run -e arbiter_mesh -e bridge -e bridge_esp32
```

A fixture-backed browser check covers mapping start/assignment/Done, RGB apply,
clock readback, and desktop/390px layouts. Before field use, verify the real
bridge/radio path, touchscreen/browser synchronization, lease expiry during an
actual disconnect, remap reports, NVS failure/reboot behavior, RTC writes, and
continued cue/event/firmware operation. No devices are flashed by these checks.

Latest local validation: 46 regression tests passed; arbiter, native-USB bridge,
classic ESP32 bridge, sensor, and screen builds all fit their app partitions.
Browser conflict handling preserves unfinished RGB input and requires review
before retrying against a new revision.

## Game goals and overview status

The top of the Installation page shows the current game/state, state age, next
transition and remaining time, named goal colour chips using the configured RGB,
sensor/screen counts, pull activity, arbiter uptime, mesh clock spread, and the
summary of desired versus reported game-device settings. Bridge connection,
report freshness, pending edits, and paused updates during firmware work are
shown alongside these values.

The Installation page shows one checkbox per game colour; ticking one sends `goals` with the new mask. `game.goal_set` is the goals as set (palette indices), whether or not the state shows them; `goal_colors` is still only what is visible.

The arbiter snapshot adds `uptime_ms`, `game`, and `mesh`. `game` includes
`ready`, `name`, `state`, `state_age_ms`, `timer_left_ms` (-1 for no timer),
`timer_target`, `goals_visible`, `goal_colors` (palette indices), `devices_ok`,
and `devices_status`. `mesh` includes `sensors`, `screens`, `events`,
`events_per_min`, and `skew_spread_ms`.

Goal component bits are converted through the game's configured major-colour
order. The arbiter LCD and snapshot use the same conversion and visibility rule:
only states reacting to goals show them. The previous round's goals are hidden
in party/idle states. The LCD shows the names as colour chips on the bottom line
of its game strip. No game rules, goals, timers, or configuration revisions are
changed by displaying this status.

The browser estimates elapsed/remaining time between fresh reports, but never
advances the state locally. On stale/disconnected/paused data it shows historical
goals and reported timer values as such. Older arbiter firmware without these
fields still supports the configuration page; the overview explains that game
status needs updated arbiter firmware. No bridge protocol change is required.

Validation: 48 host regression tests pass, including native C++ tests for hidden
goals and reordered component colours, chunked status readback, and Node tests
for timer/freshness/visibility presentation. The arbiter firmware build fits its
partition. The LCD layout and real radio goal/state updates still need hardware
verification before deployment.
