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
5. **Culture exchange** → station blends traveler's culture into its own (10% ratio)
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
11    Oscillation    1B    Frequency (maps to 0.1–5.0 Hz)
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
- `culture_random()` — generate a vivid random starting culture
- `culture_blend(a, b, ratio)` — linear interpolation between two cultures
- `culture_osc_to_hz()` / `culture_hz_to_osc()` — frequency mapping
- Stubs: `culture_mutate()`, `culture_ritual_check()`

### pulleys_patterns
**LED pattern rendering.** `PatternRenderer` class takes a `PulleysCulture` and a FastLED buffer, generates a sine-wave oscillation between the two culture colors. Per-pixel phase offset creates a traveling wave. Configurable for any LED count (10-strip station or 64-matrix traveler).

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

Thresholds (tunable):
| Zone | RSSI |
|------|------|
| CLOSE | ≥ -55 dBm |
| NEAR | ≥ -75 dBm |
| FAR | ≥ -90 dBm |
| GONE | < -90 dBm or timeout |

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
