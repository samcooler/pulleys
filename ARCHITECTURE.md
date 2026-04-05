# Pulleys — Software Architecture

## System Overview

```
┌─────────────────────────────────┐      BLE adv      ┌─────────────────────────────────┐
│          TRAVELER               │  ──────────────►   │           STATION               │
│                                 │   (16B mfr data)   │                                 │
│  ┌──────────┐  ┌─────────────┐  │                    │  ┌──────────┐  ┌─────────────┐  │
│  │ Culture  │──│  Patterns   │──│──► 8×8 LED matrix  │  │ Culture  │──│  Patterns   │──│──► 10 LED strip
│  └──────────┘  └─────────────┘  │                    │  └──────────┘  └─────────────┘  │
│       │                         │                    │       ▲                         │
│  ┌──────────┐  ┌─────────────┐  │   BLE scan (passive)  ┌──────────┐                  │
│  │ Identity │  │   Ritual    │  │  ◄──────────────   │  │Proximity │                  │
│  └──────────┘  │  (IMU stub) │  │                    │  │ Tracker  │                  │
│                └─────────────┘  │                    │  └──────────┘                  │
└─────────────────────────────────┘                    └─────────────────────────────────┘
        ▲                                                       │
        │              culture blend on CLOSE                   │
        └───────────────────────────────────────────────────────┘
```

## Data Flow

1. **Traveler boots** → generates random culture → begins advertising via BLE
2. **Station boots** → generates random culture → begins scanning for travelers
3. **Station receives BLE advertisement** → parses packet → feeds RSSI to ProximityTracker
4. **ProximityTracker classifies zone** → when traveler enters CLOSE zone, fires callback
5. **Culture exchange** → station blends traveler's culture into its own (10% ratio, 30-second per-traveler cooldown)
6. **Pattern renderer** → continuously maps current culture to LED output on both devices

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
**LED pattern rendering.** `PatternRenderer` class takes a `PulleysCulture` and a FastLED buffer. Layers of visual effect:

1. **Two-color wave** — sharpened sine oscillation between colorA and colorB (15% solid A / 70% smooth transition / 15% solid B). Per-pixel spatial phase creates a traveling wave.
2. **Radial ripple** — "stone in water" modulation with a wandering center (Lissajous path across the middle 2/3 of the matrix). Symmetric speed drift via layered sines ("woozy" effect, ±0.30 max). Spatial frequency drifts 0.5–3.1.
3. **Sparkle overlay** — rare per-pixel ignition (configurable density, default 0.2 for traveler) with brightness 180 that decays to a floor of 40. Creates organic glitter.

Configurable for any LED count (10-strip station or 64-matrix traveler). Matrix geometry (rows × cols) set via `setMatrixSize()`.

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
| CLOSE | ≥ -63 dBm | ~0.5 m |
| NEAR | ≥ -78 dBm | ~3 m |
| FAR | ≥ -85 dBm | ~6 m |
| GONE | < -85 dBm or timeout | 6m+ |

### pulleys_ritual (stub)
**IMU gesture detection.** Framework for detecting accelerometer/gyroscope gestures on Travelers (QMI8658 6-axis IMU). Planned gestures:
- SHAKE — high accel variance → amplify culture exchange
- SPIN — sustained gyro rotation → mutate colors
- HOLD_STILL — deliberate calm → deepen blend

Currently a stub; `RitualDetector.update()` accepts 6-axis data but returns `GESTURE_NONE`.

## Build Environments

| Environment | Board | Target | LEDs |
|-------------|-------|--------|------|
| `traveler` | ESP32-S3-Matrix (custom JSON) | Traveler | 64 (GPIO 14) |
| `station_s3` | ESP32-S3 DevKitC-1 | Station (dev) | 10 (GPIO 48) |
| `station_c3` | Seeed XIAO ESP32-C3 | Station (prod) | 10 (GPIO 2) |

All environments share the same library code. Device-specific behavior is driven by `PULLEYS_DEVICE_TYPE`, `LED_PIN`, and `LED_COUNT` build flags.

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

Station does not advertise (passive scanner only).
