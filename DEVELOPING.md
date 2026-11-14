# Developing Pulleys

## Hardware

| Role | Board | LEDs | Extras |
|------|-------|------|--------|
| **Traveler** | Waveshare ESP32-S3-Matrix | 8×8 WS2812B matrix, 64 LEDs (GPIO 14) | QMI8658 IMU, battery ADC (GPIO 2) |
| **Station (C3)** | Seeed XIAO ESP32-C3 | 8×32 WS2812B matrix, 256 LEDs (GPIO 10) | — |
| **Station (WROOM)** | D1 Mini ESP32-WROOM-32 | 8×32 WS2812B matrix, 256 LEDs (GPIO 16) | hardware FPU, CH340 USB-serial |
| **Bridge** | any ESP32-C3 (4MB, native USB) | none | USB to its Pi (or UART1, GPIO 21 TX / 20 RX, with `BRIDGE_PI_USB=0`); the Overlord's hosts the firmware AP |
| **Component state display** | D1 Mini ESP32-WROOM-32 | 8×32 WS2812B matrix, 256 LEDs (GPIO 16) | a Screen running its `components` display: one bar per game component |
| **Extractor** | D1 Mini ESP32-WROOM-32 | two 32×8 panels as a 64×8 ring + a hanging strip, one chain (GPIO 16) | a Screen running an `extractor_<colour>` display: that colour's score as liquid, filling up the strip into the tank; faint vertical lines round the ring when idle; while a round wants its colour every LED is lightly washed, the lines go bright, and the liquid is drawn over it |
| **Party lights** | ESP32 DevKit, 30-pin (classic) | none: four outputs (GPIO 32/33/25/26) to the AC light switches | a Screen running its `party` display: the room's lights, on for the victories |
| **Diorama** | D1 Mini ESP32-WROOM-32 | LED strip (GPIO 13), door-latch SSR (GPIO 18, HIGH unlocks), door switch with pull-up (GPIO 27), "free the spirits" button to ground (GPIO 25) | a Screen running its `diorama` display: fluorescent tubes, the door latch and sensor, and the button, which flashes the tubes purple and tells the game |

## Build Environments

| Env | Board | Notes |
|-----|-------|-------|
| `traveler` | ESP32-S3 custom | default |
| `station` | XIAO ESP32-C3 | default |
| `station_wroom` | D1 Mini ESP32-WROOM-32 | hardware FPU, SSAA=3 |
| `bridge` | ESP32-C3 (`seeed_xiao_esp32c3`) | mesh → Pi over native USB, default env |
| `bridge_esp32` | classic ESP32 (`esp32dev`), 4 MB | mesh → Pi over USB-UART; separate image to prevent cross-flashing a C3 binary |
| `bridge_s3` | ESP32-S3 (4 MB, native USB) | like `bridge`, on an S3 board. Not in `release_firmware.ENVS`: flash by USB (the Pi's `esptool` from apt lacks the S3 stub; use `pip install esptool` in a venv) |
| `component_state_display` | D1 Mini ESP32-WROOM-32 | the screen image pinned to its COMPONENTS display, default env |
| `screen_extractor` | D1 Mini ESP32-WROOM-32 | the screen image on the extractor surface (ring + strip), default env |
| `screen_party` | ESP32 DevKit, 30-pin (`esp32dev`) | the screen image pinned to PARTY: four AC light switches, no LEDs, default env |
| `screen_diorama` | D1 Mini ESP32-WROOM-32 | the screen image pinned to DIORAMA, on its strip, latch and door switch, default env |
| `screen_television` | Waveshare ESP32-S3-Touch-LCD-4.3 (the `arbiter_mesh` board) | its own image, not the screen's: a Screen on the mesh showing text messages on solid colours, default env |

```sh
pio run                      # build traveler + station (defaults)
pio run -e station_wroom     # build one env
```

Note: PlatformIO CLI is at `~/.platformio/penv/bin/pio` if not on your PATH.

## Firmware CLI (recommended)

Run `./fw` for help. The default transport is **OTA through the Overlord**, using
`http://overlord.local:8080` or `$OVERLORD_URL`. `--url` overrides the server and
`--usb` selects attached USB boards. These commands work from any directory.

```sh
./fw list                                      # known devices and running firmware
./fw list --scan                               # queue/wait for a fresh mesh census
./fw list --type sensor,bridge --json           # filtered, machine-readable inventory
./fw envs                                      # production environments and broad roles
./fw build --env sensor,bridge_esp32            # build only; no device writes
./fw build --id E857,EC52                       # infer the needed image(s) from devices
./fw flash --id E857,EC52                       # build sensor once, upload, flash both, wait
./fw flash --type sensor,screen                # all known nodes of these broad roles
./fw flash --env component_state_display       # exact running firmware environment
./fw flash --id E857,EC52 --dry-run             # inspect targets without building/flashing
./fw flash --id 59A4 --no-wait                  # return after queueing instead of watching
./fw watch                                     # watch the running job (else the latest); ./fw watch JOB_ID for one
./fw queue                                     # paused or not, what is running, what waits and in what order
./fw pause                                     # hold: the device being flashed finishes, then nothing new starts
./fw resume                                    # start the held queue again
./fw cancel                                    # stop the running job (./fw cancel JOB for a waiting one, --all for everything)
./fw move JOB top                              # reorder a waiting job (0 runs next)
./fw flash --id 59A4 --manifest dist/<timestamp>/manifest.json # reuse a bundle without rebuilding
./fw list --usb                                # attached boards, timestamps and identities
./fw flash --usb --id 59A4                      # build and upload over USB, verify reboot
./fw flash --usb --port /dev/cu.usbserial-0001 --env bridge_esp32 # first flash / reassignment
```

Builds show one line per firmware type (`[--]` queued, `[..]` building with a
timer, `[ok]` built, `[!!]` failed), ticked off as each finishes. The compiler's
output is hidden unless a build fails, and then that build's output is printed in
full. `fw build -v` / `fw flash -v` shows everything the compiler says instead.

`./fw flash` builds, uploads the images to the Overlord, queues the update, and then
**stays and watches the Overlord until it finishes**: a bar per device, plus the
Overlord's own words about the job (retries, timeouts, reboot checks) underneath.
A failed poll is retried, so a network blip does not end the watch. Ctrl-C
detaches without stopping anything; `./fw watch` reattaches. Exit status is
nonzero if the job fails.

**`./fw flash` with nothing selected builds only the firmware your changes touch.**
It reads `git` (the working tree against `HEAD`, staged or not, plus new files),
looks each changed file up in the table in `tools/firmware_changes.py`, and prints
a banner before building: every pattern that matched, the files that triggered it,
the firmware types it rebuilds, and what is being built and skipped. For example,
a change under `src/screen/` rebuilds the screen family; one under
`lib/pulleys_install/` (the tables every board carries) rebuilds all eleven; a
README or `overlord/` change builds nothing and `fw` says so.

```sh
./fw flash                      # only what your uncommitted changes touch
./fw flash --since origin/main  # what changed since a commit, not since HEAD
./fw flash --all                # every known node, ignoring git
./fw flash --env sensor         # you choose: git is not consulted (also --id, --type)
```

After you commit, the working tree is clean again, so use `--since` (for example
`--since HEAD~1`) to flash what a recent commit changed. The table is hand-written;
`test/test_firmware_changes.py` re-derives which libraries each firmware includes
and fails if the table says less, or if a new `src/` or `lib/` directory is missing
from it, so adding a library means adding a line there.

`--id`, `--type`, and `--env` accept comma-separated lists and repeated flags.
Values within one filter are alternatives; different filters are intersected.
IDs are case-insensitive and accept a name such as `N-E857` or `0xE857`.
An unknown requested ID or an empty selection stops before any build or flash.
Broad roles are `sensor`, `screen`, `bridge`, and `arbiter`: `screen` includes the
component, extractor, diorama and party profiles; `bridge` includes C3 and classic
ESP32 variants. A filter preserves each board's existing environment. Only an
explicit USB port plus a single `--env` assigns a different profile to a board.

Without filters, `flash` targets all known Overlord devices (or all attached USB
boards with `--usb`) and builds only their distinct environments. `build` with no
filters builds the full production set; role/env-only builds need no network or
attached devices. `build --id` resolves environments from inventory first.

Development builds allow uncommitted changes. Each invocation gets a new embedded
build identity and a schema-1 manifest under `dist/dev-…/`, including binary size,
SHA-256, MD5, and build identity. This also makes a deliberate repeat development
flash eligible for OTA instead of being skipped as the same build. Each environment
is built once per command, even when many boards share it. USB uploads reuse that
built image; OTA uploads send only needed binaries and queue **the resolved IDs**,
so another device appearing during the build cannot silently join the selection.

`list` shows the Overlord's last-known inventory and age of each report;
`list --scan` waits for a scan in the shared firmware queue. OTA `flash` waits for
the job and reboot/hash verification by default, returns nonzero on failure, and
supports `--no-wait` for background work. Interrupting the CLI does not cancel an
already queued Overlord job. On interactive terminals, image uploads and OTA
jobs use a fixed display with one progress bar per image/device and the percent
on the right. Rows show queued, offering, connecting, transferring, verifying,
confirmed, skipped, or failed states, including retry attempts. Transfer bars
measure bytes, so 100% still waits for reboot/hash verification; skipped devices
show `--%` because no transfer was needed. The display fits the terminal and
keeps active work visible when there are more devices than rows. Output piped to
a file, a non-terminal, or `TERM=dumb` uses plain progress lines with no cursor
control codes. Ctrl+C restores the cursor and leaves queued OTA work running.
USB flashing keeps PlatformIO's native progress output.

USB requires pyserial in the Python environment
running `fw`; PlatformIO is discovered on PATH or under `~/.platformio/penv/`, or
can be selected with `PLATFORMIO_CMD`.

### Build batches and versions

There are no semantic versions and no GitHub Releases. Every `./fw build` or
`./fw flash` is one batch, stamped with the Unix time it was triggered. Every
image in the batch shares that timestamp (the manifest's `tag` and `built_at`),
and the bundle lands in `dist/<timestamp>/`. Each image also embeds its own
build hash, which is what a device reports and what the Overlord matches.

The Overlord dashboard and `./fw list` show a build's hash and its date and
time in Pacific time (PST/PDT). To build every production image as one batch
without flashing: `python3 tools/release_firmware.py` (`--env` narrows it).
Builds never require a clean tree and nothing is published.

The older scripts remain available:

| Script | Purpose |
| --- | --- |
| `tools/release_firmware.py` | Bundle builder shared with the CLI; run directly it builds every production env as one batch |
| `tools/push_firmware.py` | Upload an already-built manifest/binaries and queue selected API updates |
| `flash_all.sh` | Legacy USB flashing plus install-map provisioning, reset and mesh survey workflows |
| `go.sh` | Legacy build → USB flash → serial monitor workflow |
| `tools/identify.py`, `tools/survey.py` | Lower-level USB identity and mesh census helpers |

## Firmware releases and updates

Firmware is pushed to the Overlord (`./fw flash`, above); the Overlord never
downloads any and does nothing about firmware at startup. It verifies asset
SHA-256 values and updates the selected nodes sequentially. The bridge's open `pulleys-fw` AP remains on
`MESH_WIFI_CHANNEL`; node downloads are served from firmware chunks requested
from the Overlord over UART.

`./flash_all.sh --ota` is retired; updates are pushed with `./fw flash`.

The first migration requires one USB flash of the new bridge environment,
because the old bridge image cannot receive the new serial update protocol:

```sh
pio run -e bridge -t upload --upload-port /dev/ttyACM0
```

The new bridge retains the old mesh invitation and `/firmware.bin` route long
enough to migrate nodes that still run the legacy OTA client.

Firmware coordinator and chunk-protocol regression tests (Python plus a C++ compiler):

```sh
python3 -m unittest discover -s test -p 'test_firmware*.py'
```

## Flashing & Monitoring

```sh
./go.sh station_wroom    # build + flash + monitor in one shot
./go.sh traveler
./go.sh station
```

Or separately:

```sh
pio run -e station_wroom          # build only
./flash_all.sh station_wroom      # flash all connected boards (requires prior build)
pio device monitor -e station_wroom   # monitor only
```

Close the monitor before flashing — it locks the serial port:
```sh
pkill -f "platformio device monitor"
```

### Serial Watch (Light Sleep)

ESP32-S3 USB CDC serial disconnects during light sleep. The standard PlatformIO monitor can't handle this. Use `serial_watch.sh` instead — it auto-reconnects by polling for `/dev/cu.usbmodem*` every 100ms:

```bash
./serial_watch.sh
```

## Identifying Your Boards

### USB Ports on macOS

```bash
ls /dev/cu.usb*
```

Common patterns:
- `/dev/cu.usbmodem*` — ESP32-S3 in USB CDC mode (traveler)
- `/dev/cu.usbserial-*` — UART bridge boards (some stations)
- `/dev/cu.wchusbserial*` — CH340 UART (XIAO C3)

### Pinning Upload Ports

If you have multiple boards connected, pin each environment to its port in `platformio.ini`:

```ini
[env:traveler]
upload_port = /dev/cu.usbmodem1101

[env:station_s3]
upload_port = /dev/cu.usbserial-0001
```

### Device Identity

Each board derives a stable 16-bit ID from its Bluetooth MAC address. On boot, the serial output shows:

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  PULLEYS Traveler
  ID:  T-A3F2 (0xA3F2)
  MAC: 68:B6:B3:xx:xx:xx
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**Label your boards** with tape/marker: write the ID (e.g. `T-A3F2`) on each one after first boot. This lets you identify them in serial logs and BLE scans.

## Labeling Workflow for Multiple Boards

1. Flash traveler firmware to a board
2. Open serial monitor, note the printed ID (e.g. `T-A3F2`)
3. Stick a label on the board with that ID
4. Repeat for all travelers, then flash stations

## Project Structure

```
pulleys/
├── platformio.ini          # Build environments & library deps
├── boards/                 # Custom board definitions
├── lib/
│   ├── pulleys_protocol/   # BLE packet format (shared)
│   ├── pulleys_identity/   # Device ID from MAC (shared)
│   ├── pulleys_culture/    # Culture data model (shared)
│   ├── pulleys_patterns/   # LED pattern renderer (shared)
│   ├── pulleys_proximity/  # RSSI proximity tracker (shared)
│   ├── pulleys_ritual/     # IMU gesture detection (traveler, stub)
│   └── pulleys_imu/        # QMI8658 accelerometer + WoM driver (traveler)
├── src/
│   ├── traveler/main.cpp   # Traveler firmware
│   ├── station/main.cpp    # Station firmware
│   └── bridge/main.cpp     # Bridge: mesh events → Pi over USB
├── flash_all.sh            # Parallel flash script
├── serial_watch.sh         # Auto-reconnecting serial monitor
└── docs (in root)
    ├── DEVELOPING.md       # This file
    ├── ARCHITECTURE.md     # System design
    └── MANIFESTO.md        # Art + tech vision
```

## Bridge (mesh → Raspberry Pi)

The `bridge` node joins the ESP-NOW mesh like any other role (it relays, answers
census and serves firmware) and forwards every deduped sensor event to the Overlord
as one text line on a second serial port. Source: `src/bridge/main.cpp`.

```sh
pio run -e bridge                                              # build
pio run -e bridge -t upload --upload-port /dev/cu.usbmodemXXXX # flash
```

Use `bridge` for an ESP32-C3, or `bridge_esp32` for a classic ESP32 with a USB-UART adapter. Both share their USB console with the Pi protocol. `flash_all.sh` maps `bridge` to the `c3`
hardware class, but its listing and converge pass skip C3 boards as "retired"
unless an env is named, so flash a fresh board with the command above or
`./flash_all.sh bridge -p <port>`. Once a board reports `env=bridge` it is kept.

### The Pi link

One USB cable from the Pi to the C3's native USB port: it powers the bridge and
carries everything, and the Pi sees it as `/dev/ttyACM0` (`/dev/cu.usbmodem*` on
a Mac). `overlord/serial_reader.py` picks a ttyACM port ahead of the Pi's GPIO
UART. The same port is the bridge's console, so the two share one stream:

- the Pi's lines all start `FW-` (the firmware protocol) or `GAME-` (tuning
  the game); anything else typed is a console command (`?`, `CENSUS`, `e3`,
  `v`, ...)
- the bridge's console output never looks like a protocol line: the cue echo,
  the `[PI]` echo and the census mirror are off in this mode, and the Overlord
  logs the rest (banner, status) as `bridge: ...` and acts on none of it
- writes to USB never wait, so a Pi that is not reading costs dropped lines,
  not a stalled radio

The UART wiring is still there for a Pi that needs it: build with
`-D BRIDGE_PI_USB=0` and the link moves to UART1, GPIO 21 = TX, GPIO 20 = RX,
115200 8N1, with the USB port back to being a console alone. Wire bridge TX (21)
→ Pi RX, bridge RX (20) ← Pi TX, ground to ground (3.3 V both sides); on the Pi
the port is `/dev/serial0` (enable the UART in `raspi-config`). Do not use GPIO
18/19: they are the C3's USB data lines. Pins and baud can be overridden with
`-D BRIDGE_TX_PIN`, `BRIDGE_RX_PIN` and `BRIDGE_BAUD`.

### Line format

```
EV <channel> <mode> <magnitude> <originId hex> <flags>\n
```

e.g. `EV 3 1 90 A855 0`: channel 3, linear mode (`0` = rotation, `1` = linear),
magnitude 90, from sensor `0xA855`, flags 0 (bit 0 = battery low). Fields are
space-separated and positional; new fields are appended. Events are deduped by
the mesh, so one pull yields one line however many copies were heard. In UART
mode, lines the Pi writes back are echoed to the console as `[PI] ...`.

Read it from a shell:

```sh
stty -F /dev/ttyACM0 115200 raw -echo && cat /dev/ttyACM0     # Linux / Pi
stty -f /dev/cu.usbmodemXXXX 115200 raw -echo && cat /dev/cu.usbmodemXXXX   # macOS
```

Test without a sensor: type `e<channel>` (for example `e3`) on the console to
inject a fake event, and it comes out on the Pi link. The console also answers
`?` (whoami) and `CENSUS`, and prints `forwarded=` / `dropped=` every 3 s. The
bridge queues up to 32 events and counts any it has to drop if the link backs up.

The arbiter's game fires cues, and the bridge writes each to the Pi link as a
line of its own:

```
cue <name>\n
cue <name> enter\n
```

e.g. `cue audio_once_chime`; the second form is a cue fired on entering a game state. On the air a cue is its name in ASCII, and the bridge
passes it on as it arrived and checks nothing: it holds no list of cues, so
changing the game's cues never needs the bridge reflashed. Test one without the arbiter by typing
`c audio_once_chime` on the console. The console shows
only `cue <name>` lines by default; `v` toggles the per-event lines and counters.

What the game does, for the Overlord's play log, comes from the arbiter as
`MESH_MSG_GAME_INFO` floods and goes to the Pi as a line each:

```
GI <key=value> ...\n
```

e.g. `GI state=easy_round goals=red-blue count.score=3`. The keys
(`start`, `state`, `goals`, `palette`, `count.<name>`, `param.<name>` in
thousandths, and `hb.*` for the 15 s heartbeat) are listed in
`overlord/playlog.py`. The other way, a line from the Pi

```
GAME-PARAM <name> <value>\n
```

goes onto the mesh as `MESH_MSG_GAME_PARAM` and sets one of the game's params
live on the arbiter, which clamps it to the game file's range (durations in
seconds). Overlord's live director and its `POST /param/<name>/<value>` route
send these.

The Overlord player and boot-time firmware coordinator live in `overlord/`.
The bridge REST firmware route is `GET /api/v1/firmware/<sha256>` on the
`pulleys-fw` AP, which only the Overlord's bridge raises (once the Overlord
starts firmware work). Each response is pulled from the Overlord over the Pi
link in bounded, checksummed chunks.

## Tips

- **Serial is verbose** — all subsystems log freely. This is intentional during development.
- **Culture starts random** — each board generates a random culture on boot. Watch the serial output for the starting colors/frequency.
- **Boot color preview** — the traveler shows its two culture colors as two solid rows in the center of the matrix for 1 second on boot, for visual verification before the pattern starts.
- **Proximity zones** — serial logs zone transitions (FAR → NEAR → CLOSE → GONE). Station prints a summary of all tracked travelers every 2 seconds.
- **Mating cooldown** — a station can only absorb culture from the same traveler once every 30 seconds, to prevent over-blending from a single visitor.
- **TX power** — traveler broadcasts at -6 dBm (reduced from default). This tightens proximity zones and saves battery. Changing TX power requires recalibrating RSSI thresholds via walk tests.
- **FastLED brightness** — capped at 32 (traveler) / 15 (station) out of 255. Lower values improve color saturation on dense LED matrices. Adjust `MAX_BRIGHTNESS` in the source if needed.
- **Threshold tuning** — walk a traveler toward the station and watch serial for `[PROX] T-XXXX: FAR → CLOSE`. Adjust `PULLEYS_RSSI_CLOSE` in the station env's `build_flags` in `platformio.ini` (default -58 dBm). NEAR and FAR track CLOSE automatically. See `FIELD.md` for field procedure.
- **Sleep/wake** — travelers enter light sleep after 30s of no motion. USB serial disconnects during sleep (ESP32-S3 CDC limitation). Use `serial_watch.sh` for auto-reconnecting serial output.
- **Debug flashes** — set `DEBUG_FLASH = true` in traveler `main.cpp` to enable color-coded LED flashes for sleep/wake debugging (red=sleep entry, green=GPIO wake, blue=timer wake, yellow=false trigger, white=exit sleep loop).
- **Battery indicator** — when a traveler enters sleep, it flashes 0–4 dim red LEDs showing battery level (0 = >80%, 4 = <20%). This always shows regardless of DEBUG_FLASH.
- **Dream state** — sleeping travelers periodically wake for a 2-second LED "dream" pattern, then return to sleep. BLE stays off during dreams. Dream interval is ~30s with random jitter.
- **Station 4-slot display** — the station's 8×32 matrix runs 4 independent 8×8 culture slots, each showing the culture of a recent visitor with a multi-phase fade transition.

## Arbiter game (state machine)

`arbiter_mesh` runs the game in `game_state_machine/state_config.yaml` and shows
it on the strip along the bottom of the panel: the current state, time in state
and the next timer, a bar that flashes on each trigger, and a short log.

- **Default game:** baked in at build time by `tools/embed_game.py` (a PlatformIO
  pre-script on `arbiter_mesh`) as a generated `game_default.h`. A bad
  YAML fails the build.
- **Replacing it:** join the `pulleys-arbiter` Wi-Fi, then
  `python3 tools/game_to_json.py --post 192.168.4.1:8080`. See
  `src/arbiter_mesh/gameconfig.h` for `/config`, `/event`, `/state`. The game
  restarts at its `start` state. A posted game is kept across reboots until
  `DELETE /config`; the game's *state* never is.
- **Events:** `components.cpp` turns sensor events into game events (placeholder
  rules, mirroring `overlord/simulate.py`) and hands them to `gamestate_event()`.
  Inject one by hand with `g component_activated` on the console, or `POST /event`.
- **Cues:** go out as mesh packets for the bridge (above). A game POSTed with a
  different cue list needs no bridge change: cues travel by name.

## Component state display

Not a role of its own: one of a Screen's displays, `components` (`SCREEN_COMPONENTS`).
Every screen hears the same mesh -- sensor events, the arbiter's palette, the
component charges -- so any screen can run it: name the board in
`DISPLAY_ASSIGNMENT` in `lib/pulleys_install` with `SCREEN_COMPONENTS`, or press
`m` on its console to step to it for one boot. The `component_state_display` env
is the screen image with this display pinned, kept for the board already
installed as one (the firmware updater only offers a board the env it reports).

Eight vertical bars side by side along the long axis, each 3 pixels wide with a
1-pixel gap (31 of the 32 columns): the five major components on the left, in
the game's colour order, then up to three finals (purple). Each bar wears the
colour the arbiter sends with it, and shows its component's charge: pulls
raise it, it drains the whole time, and the component lights when the bar is
full and goes dark when it is empty again. Source:
`renderComponents` in `src/screen/main.cpp`; the bar maths is
`lib/pulleys_component_display`.

```sh
pio run -e component_state_display -t upload --upload-port /dev/cu.usbserial-XXXX
./flash_all.sh component_state_display -p <port>
```

- **Where the state comes from:** the arbiter. `components.cpp` there owns the
  rules (which channel is which component, and how fast a lit one drains: the
  game's `components` and `default_decay_rate`, see `GAME_FORMAT.md`), and publishes every
  component's charge as a percentage (0-100) together with that rate in a
  `MeshComponents` packet: right away on any change, and once a second otherwise.
  The display only draws it, draining each bar at the published rate between
  packets, so a missed packet costs nothing and the display never holds a drain
  time of its own.
- **No arbiter:** if nothing has been heard for 4 s, a pixel in the spare column at the right end blinks red.
- **Orientation:** if the bars come out mirrored (component 1 on the right) or
  fill downward, add `-D COMPONENT_DISPLAY_REVERSE_X=1` and/or
  `-D COMPONENT_DISPLAY_REVERSE_Y=1` to the env's `build_flags`. Brightness is
  `COMPONENT_BRIGHTNESS` in `src/screen/main.cpp` (60 of 255) and the current cap
  `LED_MAX_MA` (450 mA, USB-safe; raise it on a real 5 V supply).
- **Bench:** `c<bar> <percent>` on the console draws a bar as if the arbiter had
  sent it (`c0 100` fills component 1 and lets it drain). On the arbiter, `p<channel>`
  injects a pull (`p1` lights major component 1, `p10` major component 2), which is how to see
  it without a sensor.

## Extractors

Five screens, one per colour except purple, each showing that colour's score as
a column of liquid. The surface is one chain: two 32×8 panels bent into a 64×8
ring (512 LEDs), then a strip hanging below it, its first LED at the top. The
liquid fills the strip from its far (bottom) end, then the ring from the bottom
row up; every pixel runs from off to the full colour. In the strip the surface
bobs; in the ring it becomes waves -- summed sines that wrap once round the 64
columns -- and pouring kicks up the slosh, which settles over a few seconds.

The score is the charge of the major component wearing that colour, as the
arbiter publishes it for the component bars (pulls add to it, it drains at the
game's rate). `c<bar> <percent>` on the console drives it on the bench.

While a round wants the colour, the ring's top and bottom rows are outlined in
it at half brightness: an empty tank waiting to be filled. The outline stays
while the colour is a goal, full or not, since a full tank can drain. The
arbiter sends the goals in the components packet (its `goals` byte), only
while the state reacts to goal events. `g<mask>` on the console fakes them
(bit i = component i). Until every board runs firmware with that byte, a
relay through an older node drops it and the outline goes out.

Built with `screen_extractor`; it runs only the extractor displays, and a plain
8×32 screen refuses them. Pick the colour in `DISPLAY_ASSIGNMENT` with
`SCREEN_EXTRACTOR_RED` .. `_ORANGE`; an unlisted extractor steps to the next
colour each power cycle, and `m` steps it for one boot. Set the strip's length
with `EXTRACTOR_STRIP_LEDS` (and `LED_COUNT` = 512 + it) in `platformio.ini`;
flip `EXTRACTOR_REVERSE_Y` / `EXTRACTOR_STRIP_REVERSE` if either fills the wrong
way. The liquid maths and wiring are `lib/pulleys_extractor`; the render is
`renderExtractor` in `src/screen/main.cpp`.

## Sensors and rough play

A sensor counts a pull and then ignores its rope for the detector's 1.5 s
cooldown (`pulleys_detect`), so one rope gives at most one pull every 1.7 s
or so, however hard it is worked. Game balance (`charge_per_pull`) has to
assume that pace.

Rough handling makes a rope go dead for a moment. `RoughGuard` reads every IMU
sample, cooldown included: spun faster than 1000 deg/s, or jerked more than
2 g from gravity, the rope is overstimulated until it has been under both for
2.5 s. Meanwhile a detection does not count (no event goes out, `✗ ROUGH` on
the console) and the handle goes dark at once, idle candle included. The
limits are guesses: the status line prints `[ROUGH]` with the peak rate and
acceleration since the last line, `R<dps> <g>` on the console tries others
until reboot, and `R0` turns the guard off. Put the ones that feel right into
`RoughConfig`.

## Diorama

One board: a Screen built as `screen_diorama`, with an LED strip, a solid-state
relay on the door latch and a switch on the door. The strip is cut, in light,
into fluorescent tubes -- 8 pixels lit (5 in at 60/m), 3 dark (2 in) -- in a
cold blue-green white. Each tube strikes up with a stutter, hums, and drops out
now and then; one is the bad tube and flickers far more. The tube model is
`lib/pulleys_diorama`; the board side is the DIORAMA section of
`src/screen/main.cpp`.

The game drives it through device settings (see Device settings below):
`diorama.lights` (`dark` | `creepy` | `paradise`) and `diorama.latch`
(`locked` | `open`). Today's game turns both on 10 s into `party` (game 1 won),
with an `at:` step. On entering `victory2` (game 2 won) the latch locks and the
lights go to `paradise`: the same tube spacing, but slow rainbow fades that
overlap, each tube breathing on its own phase. `kickout` and `passive` turn
them dark and locked. It hears `diorama.door_opened` and `diorama.door_closed` back.

Opening the door while the lights are creepy also makes the tubes stutter a
dim red for 2 s, on the board so it is instant, then they shudder back to
their flicker. The latch is driven locked from the first line of `setup()`.

Wiring and polarity are build flags in `[env:screen_diorama]`: `DIORAMA_LEDS`
(and `LED_COUNT`), `DIORAMA_LATCH_PIN` / `DIORAMA_DOOR_PIN`,
`DIORAMA_LATCH_UNLOCK` (HIGH: power opens it, a strike; LOW: power holds it, a
maglock) and `DIORAMA_DOOR_OPEN` (HIGH: the switch closes to ground when the
door is shut). On the console, `d` toggles a bench override (lit and unlatched
whatever the game says), `o` acts as if the door opened, `t r,g,b` tunes the
tint live, and `S diorama.lights=creepy` (or `=paradise`) behaves as if the arbiter had set
it.

## Party lights

One board, a classic 30-pin ESP32 DevKit built as `screen_party`, with no LEDs:
four outputs (GPIO 32, 33, 25, 26), each driving a switch for a string of the
room's AC lights, driven by one device setting, `party.lights` (`dark` |
`lit`). Today's game lights them on entering `party` (game 1 won) and
`victory2` (game 2 won), and darkens them on entering `final`, `kickout` and
`passive`. The four come on 200 ms apart, to spread the inrush.

Every output is driven off as the first thing on boot, and the DevKit's blue
LED (GPIO 2) shows whether the lights are on. For an active-low relay board,
build with `-D PARTY_ON=LOW`; the pins are `PARTY_PINS` in
`[env:screen_party]`. On the console, `p` toggles a bench override (lit
whatever the game says) and `S party.lights=lit` behaves as if the arbiter had
set it.

## Television

One board: the TV set inside the diorama, built as `screen_television` on the
same Waveshare 4.3" LCD board the arbiter uses. It is a Screen on the mesh --
it relays, answers census as `television`, and takes firmware updates -- but
it is its own firmware (`src/television`), not the screen image, because its
surface is an 800x480 LCD rather than LEDs. The panel driver is
`lib/pulleys_lcd`, shared with the arbiter.

It shows one message at a time: text, wrapped, centred and as large as fits,
on a solid colour. The messages are the `TV_MESSAGES` table in
`src/television/messages.h`, and each is on a channel: text (`\n` forces a line
break), background and text colour as `0xRRGGBB`, and seconds to hold. The set
plays its channel's messages in order, fading through black between them, and
starts over at the end; a channel of one message just holds it.

The game picks the channel with one device setting (see Device settings
below), `television.channel`: a channel name, or `dark` for the screen off,
backlight and all. A change fades straight to the new channel's first message.
Unset -- no game yet, or the arbiter silent past the lease -- it plays
`TV_DEFAULT_CHANNEL` (`idle`). It reports the channel it is showing back to the
arbiter. The values listed for `television.channel` in the game file must be
exactly the firmware's channels plus `dark`; `test/test_television.py` checks
that, so adding a channel means adding it in both places.

On the console, `n` changes to the next message on the channel now, `b` goes
back one, `h` holds on the current one, `c` lists the channels, and
`S television.channel=janis` behaves as if the arbiter had set it.
`-D TV_FADE_MS=0` makes every change a hard cut.

## Queue sign

The arbiter's screen doubles as the sign at the front of the installation.
From boot it shows a text card for the queue instead of the monitor: words on
a solid colour, drawn the same way as the television's (`lib/pulleys_textcard`).
The game picks what it says with the device setting `queue.sign`, a setting of
the arbiter itself rather than of a device on the mesh; today's game sets
`enter` in `passive` and `wait` from the start of `easy_round` until it is back
in `passive`. The words for each value are `SIGN_MESSAGES` in
`src/arbiter_mesh/sign_messages.h`, a channel per value like the television's
(`test/test_television.py` checks they match the game file).

While the sign is up it has the whole screen and LVGL is paused, so the
monitor's buttons are out of the public's reach. To get to the monitor, tap
the screen's four quadrants in the secret order -- by default top left, bottom
right, bottom left, top right, each within 3 s of the last
(`ARBITER_SIGN_UNLOCK` in `src/arbiter_mesh/signui.cpp`). Nothing on screen
answers a tap. The monitor has a red SIGN button at the top right to put the
sign back, and the sign returns by itself after 5 minutes with no touch and no
install screen open. `Q` on the serial console swaps them without the taps.
A game with no `queue.sign` has no sign: the monitor stays up.

## Device settings

How the game drives things in the room that are not media -- lights, latches --
and hears back from their switches. The game file declares each device under
`devices:` and changes settings with `set:` (GAME_FORMAT.md). Unlike a cue,
which happens once, a setting holds until the game changes it.

- **Arbiter → devices.** `src/arbiter_mesh/devices.cpp` keeps the table and
  broadcasts all of it (`MESH_MSG_STATE`) on every change, as a short burst, and
  once a second. A device that missed a packet or rebooted is right within a
  second.
- **The lease.** A device that has heard no table for 12 s
  (`SETTINGS_LEASE_MS`) treats every setting as unset and falls back to its own
  safe state. Losing the arbiter turns the lights out and locks the door; it
  never leaves the room stuck.
- **Devices → arbiter** (`MESH_MSG_DEVICE`). An input (the diorama's door) goes
  out as a burst and reaches the game as an event, `diorama.door_opened`. A
  device also reports what it is actually doing: on change, and every 20 s
  otherwise -- not every second, because every node relays every packet and
  per-second reports from every device would cost the mesh the square of its
  size. On the arbiter's console `V` lists each setting, wanted against
  reported; `/state` includes the same under `devices`; and the game panel's
  bottom-right line reads "devices: 3/3 as set", or names the first that is
  not (amber), including one not heard from in 60 s.
- **On the air** a name or value is a 16-bit hash (`lib/pulleys_settings`, and
  `name_hash` in `tools/game_to_json.py`, which refuses collisions).
- **Relay.** Message types `0xA0` and up share one header and every node
  relays them unread (`pulleys_mesh.h`, "Flood"), so a new type needs only its
  sender and receivers updated -- after this release. Boards running firmware
  from before it drop these packets, so update every board once.

A new device: declare it under `devices:` in the game file, and in its firmware
call `pulleys::settings_init()` after `mesh_init()`, read
`pulleys::setting_is("dev.setting", "value")` every loop (unset means safe),
`device_report()` what it is doing, `device_emit()` its inputs, and
`settings_poll()` every loop.

## Bridge transfer speed

Bridge serial defaults to 921600 baud; firmware chunks are 4 KiB with per-chunk
CRC-32. Deploy Overlord's matching host code before updating the bridge: its
reader probes both fast and legacy 115200 rates and reconnects after the bridge
self-update changes its boot speed. Other roles keep their 115200 consoles.
See `overlord/FIRMWARE_API.md` for framing and `overlord/README.md` for migration
and the non-flashing transfer benchmark.
