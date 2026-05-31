# Pulleys — Software Architecture

## System Overview

```
┌──────────────────────────────────────────┐     BLE adv      ┌──────────────────────────────────┐
│              TRAVELER                    │  ────────────►   │            STATION               │
│                                          │  (16B mfr data)  │                                  │
│  ┌──────────┐  ┌──────────┐              │                  │  ┌──────────┐  ┌──────────┐      │
│  │ Culture  │──│ Patterns │──► 8×8 LEDs  │                  │  │ Culture  │──│ Patterns │──► 8×32 LEDs
│  └──────────┘  └──────────┘   (64, RGB)  │                  │  │ (×4 slots)│  │(seesaw)  │  (256, RGB)
│       │                                  │                  │  └──────────┘  └──────────┘      │
│  ┌──────────┐  ┌──────────┐              │  BLE scan        │       ▲                          │
│  │ Identity │  │   IMU    │◄── QMI8658   │  (passive)       │  ┌──────────┐                    │
│  └──────────┘  │  (WoM)   │   INT1/INT2  │  ◄───────────    │  │Proximity │                    │
│                └──────────┘              │                  │  │ Tracker  │                    │
│  ┌──────────┐  ┌──────────┐              │                  │  └──────────┘                    │
│  │  Sleep   │  │ Battery  │◄── ADC GPIO2 │                  │                                  │
│  │  State   │  │ Monitor  │              │                  │                                  │
│  │ Machine  │  └──────────┘              │                  │                                  │
│  └──────────┘                            │                  │                                  │
└──────────────────────────────────────────┘                  └──────────────────────────────────┘
        ▲                                                              │
        │                 culture blend on CLOSE                       │
        └──────────────────────────────────────────────────────────────┘
```

## Data Flow

1. **Traveler boots** → generates random culture → begins advertising via BLE → shows boot color preview
2. **Station boots** → generates 4 random cultures (one per slot) → begins scanning for travelers
3. **Station receives BLE advertisement** → parses packet → feeds RSSI to ProximityTracker
4. **ProximityTracker classifies zone** → when traveler enters CLOSE zone, fires callback
5. **Culture exchange** → a random station slot adopts the traveler's culture with a multi-phase transition (1s fade-old-to-black → 1s fade-new-in at 3× brightness → 4s settle to 1×). 30-second per-traveler cooldown.
6. **Pattern renderer** → continuously maps current culture to LED output on both devices
7. **Traveler sleeps** → after 30s of no motion, fades out → shows battery indicator → enters light sleep with WoM interrupt
8. **Traveler dreams** → periodic timer wakes the traveler for a brief 2s LED pattern, then back to sleep
9. **Traveler wakes** → motion triggers WoM interrupt → fade in → resume BLE advertising

## BLE Packet Format

16 bytes of manufacturer-specific data in the BLE advertisement:

```
Byte  Field          Size  Description
────  ─────          ────  ───────────
0–1   Company ID     2B    0xFFFF (development)
2     Device type    1B    0x01=Station, 0x02=Traveler
3–4   Device ID      2B    Stable 16-bit hash of BT MAC
5–7   Color A        3B    RGB
8–10  Color B        3B    RGB
11    Oscillation    1B    Frequency (maps to 0.02–0.5 Hz)
12–15 Counter        4B    Monotonic sequence number
────
Total: 16 bytes (fits within 31-byte BLE adv payload)
```

## Components

### pulleys_protocol
**Shared BLE packet format.** Defines `PulleysPacket` struct and `pulleys_serialize()` / `pulleys_parse()` functions. Both Station and Traveler use this to encode/decode the manufacturer data in BLE advertisements.

### pulleys_identity
**Device identification.** Derives a stable 16-bit ID from the ESP32 Bluetooth MAC address via multiplicative hash. Provides human-readable names (`T-A3F2`, `S-7B01`) and a serial boot banner. The ID is embedded in every BLE packet for tracking.

### pulleys_culture
**Culture data model.** A culture is two RGB colors and an oscillation frequency byte. Provides:
- `culture_random()` — generate a vivid random starting culture via HSV with high saturation (≥200) and minimum 72° hue separation between the two colors
- `culture_blend(a, b, ratio)` — linear interpolation between two cultures
- `culture_osc_to_hz()` / `culture_hz_to_osc()` — frequency mapping (byte 1–255 → 0.02–0.5 Hz, i.e. 2–50 second cycles)
- `color_name()` — approximate human-readable color name from RGB (red, orange, yellow, green, cyan, blue, purple, pink, magenta, teal, white, black, mix)
- `culture_print()` — serial debug output with color names
- Stubs: `culture_mutate()`, `culture_ritual_check()`

### pulleys_patterns
**LED pattern rendering.** Two pattern types:

**PATTERN_RADIAL_RIPPLE** (traveler) — `PatternRenderer` class layers three visual effects:
1. **Two-color wave** — sharpened sine oscillation between colorA and colorB (15% solid A / 70% smooth transition / 15% solid B). Per-pixel spatial phase creates a traveling wave.
2. **Radial ripple** — "stone in water" modulation with a wandering center (Lissajous path across the middle 2/3 of the matrix). Symmetric speed drift via layered sines ("woozy" effect, ±0.30 max). Spatial frequency drifts 0.5–3.1.
3. **Sparkle overlay** — rare per-pixel ignition (configurable density, default 0.2 for traveler) with brightness 180 that decays to a floor of 40. Creates organic glitter.

**PATTERN_PILLOW_SEESAW** (station) — antiphase left/right seesaw with cosine brightness dome. Used for the station's 4-slot 8×32 matrix, where each 8×8 slot runs an independent seesaw pattern.

Uses `Wanderer` jerk-impulse physics for smooth organic motion in both pattern types. Global brightness wandering adds slow luminance drift. Configurable for any LED count. Matrix geometry (rows × cols) set via `setMatrixSize()`.

Extension points:
- Matrix-aware patterns (radial, spiral) for traveler's 8×8 grid
- Breathing/pulsing modes for station orbs
- Pattern selection driven by culture traits

### pulleys_proximity
**RSSI-based proximity detection.** `ProximityTracker` class maintains a table of up to 32 tracked devices. For each:
- Exponential moving average of RSSI (α=0.3)
- Zone classification: GONE / FAR / NEAR / CLOSE
- Hysteresis (±5 dBm) to prevent zone flickering
- 10-second timeout for device expiry
- Callback fires on zone transitions

Thresholds (tunable, calibrated for -6 dBm TX power):
| Zone | RSSI | Approx. distance |
|------|------|-------------------|
| CLOSE | ≥ -58 dBm | ~0.5 m |
| NEAR | ≥ -73 dBm | ~3 m |
| FAR | ≥ -80 dBm | ~6 m |
| GONE | < -80 dBm or timeout | 6m+ |

### pulleys_imu
**QMI8658 accelerometer driver.** Minimal I2C driver for the QMI8658 6-axis IMU (accel-only mode, ±2g). I2C address 0x6B, SDA=GPIO11, SCL=GPIO12. Provides:
- `init()` — soft reset, configure accelerometer at 62.5Hz normal mode
- `read()` — read 3-axis acceleration, returns false if not initialized
- `configWakeOnMotion(threshold)` — configure WoM interrupt on INT1 (GPIO10): soft reset → 3Hz low-power accel → set threshold/blanking via CAL1 registers → CTRL9 command 0x08 → enable accel
- `checkWomEvent()` — poll STATUS1 bit 2 for WoM event
- `restoreNormalMode()` — proper WoM exit per datasheet section 9.6: disable sensors → clear threshold → CTRL9 command 0x08 → re-init. This releases the INT pins so WoM can be reconfigured for subsequent sleep cycles.
- `writeCtrl9Cmd(cmd)` — CTRL9 protocol with STATUS_INT bit7 handshake

INT pin routing: INT1 (GPIO10) active-low, initial state HIGH. INT2 (GPIO13) mirrors INT1 by hardware default. Both are used as ESP32-S3 GPIO wake sources.

### pulleys_ritual (stub)
**IMU gesture detection.** Framework for detecting accelerometer/gyroscope gestures on Travelers (QMI8658 6-axis IMU). Planned gestures:
- SHAKE — high accel variance → amplify culture exchange
- SPIN — sustained gyro rotation → mutate colors
- HOLD_STILL — deliberate calm → deepen blend

Currently a stub; `RitualDetector.update()` accepts 6-axis data but returns `GESTURE_NONE`.

## Sleep / Wake State Machine (Traveler)

The traveler uses ESP32-S3 light sleep to conserve battery when no one is interacting with it.

```
                    motion detected
          ┌──────────────────────────────┐
          ▼                              │
       AWAKE ──(30s no motion)──► FADING_OUT ──► battery flash ──► ASLEEP
          ▲                                                          │ │
          │                                           ┌──────────────┘ │
          │                                    timer wake         GPIO wake
          │                                           ▼              │
       FADING_IN ◄──────────────────────────────── (motion)         │
          ▲                                                          │
          │         DREAM_OUT ◄── DREAM_LIT ◄── DREAM_IN ◄──────────┘
          │              │            (2s pattern plays)
          └──────────────┘
```

**States:**
- `AWAKE` — BLE advertising, LED pattern running, IMU reading at 100ms intervals. Motion threshold: 0.05g delta from baseline.
- `FADING_OUT` — 1s brightness fade to black. Motion during fade reverses to FADING_IN. On completion, reads battery voltage and shows 0–4 red LED flashes (see Battery Monitoring).
- `ASLEEP` — ESP32-S3 light sleep. WoM configured on QMI8658 at 3Hz low-power mode. GPIO wake sources: INT1 (GPIO10) + INT2 (GPIO13), low-level trigger. Timer wake: 30s ± 5s jitter for dreams. BLE is stopped.
- `FADING_IN` — 1s brightness fade from black. On completion, transitions to AWAKE.
- `DREAM_IN` / `DREAM_LIT` / `DREAM_OUT` — brief timer-triggered wake: fade in, play pattern for 2s, fade out, return to ASLEEP. BLE stays off. IMU stays in WoM mode (no false motion from mode switching).

**Sleep timeout** uses a random value recomputed after each trigger (30s ± 5s jitter), preventing synchronized wake/sleep across multiple travelers.

## Battery Monitoring

ADC on GPIO2 with a matched resistor divider (ratio 2.0). Reads raw ADC → scales to actual voltage → maps to LiPo percentage (3.0V = 0%, 4.2V = 100%).

After FADING_OUT completes (just before entering light sleep), the traveler flashes 0–4 dim red LEDs as a battery indicator:
- 0 flashes = >80% charge
- 1 flash = 60–80%
- 2 flashes = 40–60%
- 3 flashes = 20–40%
- 4 flashes = <20%

Battery voltage (raw + scaled) is also included in the 1Hz serial status log during AWAKE.

## Power Optimization

- **WoM at 3Hz** — during sleep, the QMI8658 runs at 3Hz low-power accelerometer mode (vs 62.5Hz normal), minimizing IMU power draw
- **Light sleep** — ESP32-S3 light sleep preserves RAM state while cutting CPU/radio power
- **BLE off during sleep** — NimBLE deinit during sleep, re-init on wake
- **TX power -6 dBm** — reduced BLE transmit power saves battery and tightens proximity zones

## Build Environments

| Environment | Board | Target | LEDs |
|-------------|-------|--------|------|
| `traveler` | ESP32-S3-Matrix (custom JSON) | Traveler | 64 (GPIO 14), 8×8 matrix |
| `station` | Seeed XIAO ESP32-C3 | Station | 256 (GPIO 10), 8×32 matrix |

Both environments use Arduino framework, FastLED, and NimBLE-Arduino. All library code is shared. Device-specific behavior is driven by `PULLEYS_DEVICE_TYPE`, `LED_PIN`, and `LED_COUNT` build flags.

## Extension Points

These are marked with `TODO` comments in the code:

1. **Bidirectional culture exchange** — Travelers absorb station culture when visiting (scan callback stub in traveler main.cpp)
2. **Genetic mutation** — Random culture drift over time (`culture_mutate()` stub)
3. **Ritual interaction** — IMU gestures modify exchange dynamics (`RitualDetector` stub)
4. **Matrix patterns** — Spatial 8×8 patterns that use row/column position
5. **OTA updates** — ArduinoOTA stubs in both main.cpp files
6. **Pattern library** — Multiple pattern types selectable by culture traits
7. **Culture persistence** — Save culture to NVS flash so stations remember across reboots

## Radio Configuration

Traveler TX power is set to **-6 dBm** (vs default +3 or +9). This:
- Reduces effective BLE range, making proximity zones tighter and more precise
- Saves battery on the traveler
- Requires proximity thresholds to be calibrated against real-world walk tests

Advertising interval is 500ms. Station does not advertise (passive scanner only).

## Station Culture Display

The station's 8×32 LED matrix is divided into **4 independent slots** of 8×8 pixels each. Each slot:
- Starts with a random culture on boot
- Runs `PATTERN_PILLOW_SEESAW` independently
- On culture exchange, the target slot transitions via: 1s fade-old-to-black → 1s fade-new-in at 3× brightness → 4s settle to 1×
- Slot selection for incoming culture is random

The multi-slot design means a station shows the cultural history of its recent visitors across its entire display, rather than blending everything into a single color.
