# Developing Pulleys

## Hardware

| Role | Board | LEDs | Extras |
|------|-------|------|--------|
| **Traveler** | Waveshare ESP32-S3-Matrix | 8×8 WS2812B matrix, 64 LEDs (GPIO 14) | QMI8658 IMU, battery ADC (GPIO 2) |
| **Station (C3)** | Seeed XIAO ESP32-C3 | 8×32 WS2812B matrix, 256 LEDs (GPIO 10) | — |
| **Station (WROOM)** | D1 Mini ESP32-WROOM-32 | 8×32 WS2812B matrix, 256 LEDs (GPIO 16) | hardware FPU, CH340 USB-serial |
| **Bridge** | ESP32-C3 (4MB, native USB) | none | UART1 to the Overlord (GPIO 21 TX, 20 RX); open firmware AP on mesh channel |
| **Component state display** | D1 Mini ESP32-WROOM-32 | 8×32 WS2812B matrix, 256 LEDs (GPIO 16) | the screen's board; one bar per game component |

## Build Environments

| Env | Board | Notes |
|-----|-------|-------|
| `traveler` | ESP32-S3 custom | default |
| `station` | XIAO ESP32-C3 | default |
| `station_wroom` | D1 Mini ESP32-WROOM-32 | hardware FPU, SSAA=3 |
| `bridge` | ESP32-C3 (`seeed_xiao_esp32c3`) | mesh → Pi serial bridge, default env, always a C3 |
| `component_state_display` | D1 Mini ESP32-WROOM-32 | component charge bars on an 8×32 matrix, default env |

```sh
pio run                      # build traveler + station (defaults)
pio run -e station_wroom     # build one env
```

Note: PlatformIO CLI is at `~/.platformio/penv/bin/pio` if not on your PATH.

## Firmware releases and updates

The Overlord checks the latest GitHub Release in the background at startup,
compares release build identities, verifies asset SHA-256 values, and updates
reachable nodes sequentially. The bridge's open `pulleys-fw` AP remains on
`MESH_WIFI_CHANNEL`; node downloads are served from firmware chunks requested
from the Overlord over UART.

Publish active firmware environments with:

```sh
python3 tools/release_firmware.py
```

This builds `sensor`, `screen`, `component_state_display`, `arbiter_mesh`, and
`bridge`, then creates the next minor GitHub Release with the images and
`manifest.json`. Use `--build-only` to create local assets only. `./flash_all.sh
--ota` is retired; managed updates are initiated by restarting the Overlord.

The first migration requires one USB flash of the new bridge environment,
because the old bridge image cannot receive the new serial update protocol:

```sh
pio run -e bridge -t upload --upload-port /dev/serial0
```

The new bridge retains the old mesh invitation and `/firmware.bin` route long
enough to migrate nodes that still run the legacy OTA client.

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
│   └── bridge/main.cpp     # Bridge: mesh events → Pi over UART1
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

The bridge is always an ESP32-C3. `flash_all.sh` maps `bridge` to the `c3`
hardware class, but its listing and converge pass skip C3 boards as "retired"
unless an env is named, so flash a fresh board with the command above or
`./flash_all.sh bridge -p <port>`. Once a board reports `env=bridge` it is kept.

### Two serial ports

| Port | Where | Carries |
|------|-------|---------|
| Console | native USB (`/dev/cu.usbmodem*`) | boot banner, `★` event log, whoami / census |
| Overlord link | UART1: GPIO 21 = TX, GPIO 20 = RX, 115200 8N1 | cues/events, update control, Base64 image chunks |

They are separate on purpose, so boot banners and whoami replies never end up in
front of whatever reads the Pi link. Do not use GPIO 18/19: they are the C3's USB
data lines. Pins and baud can be overridden with `-D BRIDGE_TX_PIN`,
`BRIDGE_RX_PIN` and `BRIDGE_BAUD` in the env's `build_flags`.

Wiring to the Pi: bridge TX (21) → Pi RX, bridge RX (20) ← Pi TX, ground to
ground. Both sides are 3.3 V logic. On the Pi the port is `/dev/serial0` (enable
the UART in `raspi-config`) or `/dev/ttyUSB0` for a USB adapter.

### Line format

```
EV <channel> <mode> <magnitude> <originId hex> <flags>\n
```

e.g. `EV 3 1 90 A855 0`: channel 3, linear mode (`0` = rotation, `1` = linear),
magnitude 90, from sensor `0xA855`, flags 0 (bit 0 = battery low). Fields are
space-separated and positional; new fields are appended. Events are deduped by
the mesh, so one pull yields one line however many copies were heard. Lines the
Pi writes back are echoed to the console as `[PI] ...`.

Read it from a shell:

```sh
stty -F /dev/serial0 115200 raw -echo && cat /dev/serial0     # Linux / Pi
stty -f /dev/cu.usbserial-XXXX 115200 raw -echo && cat /dev/cu.usbserial-XXXX   # macOS adapter
```

Test without a sensor: type `e<channel>` (for example `e3`) on the console to
inject a fake event, and it comes out on the Pi link. The console also answers
`?` (whoami) and `CENSUS`, and prints `forwarded=` / `dropped=` every 3 s. The
bridge queues up to 32 events and counts any it has to drop if the UART backs up.

The arbiter's game fires cues, and the bridge writes each to the Pi link as a
line of its own:

```
cue <name>\n
```

e.g. `cue audio_once_chime`. On the air a cue is its name in ASCII, and the bridge
passes it on as it arrived and checks nothing: it holds no list of cues, so
changing the game's cues never needs the bridge reflashed. Test one without the arbiter by typing
`c audio_once_chime` on the console. The console shows
only `cue <name>` lines by default; `v` toggles the per-event lines and counters.

The Overlord player and boot-time firmware coordinator live in `overlord/`.
The bridge REST firmware route is `GET /api/v1/firmware/<sha256>` on its
continuous `pulleys-fw` AP. Each response is pulled from the Overlord through
UART1 in bounded, checksummed chunks.

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

A mesh node on the screen's hardware (D1 Mini ESP32, 8×32 matrix on GPIO 16) that
shows how charged each game component is. Eight vertical bars side by side along
the long axis, each 3 pixels wide with a 1-pixel gap (31 of the 32 columns):
major components 1-5 on the left (red, orange, hot pink, green, blue), then final
components 1-3 (purple). A component that is lit shows a full bar, which drains
away until the component goes dark and the bar is empty. Source:
`src/component_state_display/main.cpp`; the bar maths is `lib/pulleys_component_display`.

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
  `COMPONENT_DISPLAY_BRIGHTNESS` (default 60 of 255) and the current cap
  `LED_MAX_MA` (450 mA, USB-safe; raise it on a real 5 V supply).
- **Bench:** `c<bar> <percent>` on the console draws a bar as if the arbiter had
  sent it (`c0 100` fills component 1 and lets it drain). On the arbiter, `p<channel>`
  injects a pull (`p1` lights major component 1, `p10` major component 2), which is how to see
  it without a sensor.
