# Developing Pulleys

## Hardware

| Role | Board | LEDs | Extras |
|------|-------|------|--------|
| **Traveler** | Waveshare ESP32-S3-Matrix | 8×8 WS2812B (GPIO 14) | QMI8658 IMU |
| **Station (dev)** | ESP32-S3 DevKitC-1 | 10× WS2812B strip (GPIO 48) | — |
| **Station (prod)** | Seeed XIAO ESP32-C3 | 10× WS2812B strip (GPIO 2) | — |

## Build Environments

```
pio run -e traveler      # build traveler firmware
pio run -e station_s3    # build station for ESP32-S3 (dev board)
pio run -e station_c3    # build station for XIAO ESP32-C3 (production)
```

Default `pio run` builds `traveler` + `station_s3`.

## Flashing & Monitoring

One board at a time via USB-C:

```bash
# Flash traveler
pio run -e traveler -t upload

# Monitor traveler serial output
pio device monitor -e traveler

# Flash + monitor in one shot
pio run -e traveler -t upload && pio device monitor -e traveler
```

Same for station:
```bash
pio run -e station_s3 -t upload && pio device monitor -e station_s3
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
│   └── pulleys_ritual/     # IMU gesture detection (traveler)
├── src/
│   ├── traveler/main.cpp   # Traveler firmware
│   └── station/main.cpp    # Station firmware
└── docs (in root)
    ├── DEVELOPING.md       # This file
    ├── ARCHITECTURE.md     # System design
    └── MANIFESTO.md        # Art + tech vision
```

## OTA Updates (Future)

When deploying 5–20 stations in trees, USB cables aren't practical. Plan:

1. **ArduinoOTA** over WiFi — each board connects to a local WiFi AP
2. Station hostnames use their device ID: `S-7B01.local`
3. Flash over WiFi: `pio run -e station_c3 -t upload --upload-port S-7B01.local`
4. Requires adding WiFi credentials and OTA init to firmware (stubs are in place)

Alternative: ESP-IDF native OTA with HTTP server for fleet updates.

## Tips

- **Serial is verbose** — all subsystems log freely. This is intentional during development.
- **Culture starts random** — each board generates a random culture on boot. Watch the serial output for the starting colors/frequency.
- **Proximity zones** — serial logs zone transitions (FAR → NEAR → CLOSE → GONE). Useful for tuning RSSI thresholds.
- **FastLED brightness** — capped at 50 (traveler) / 80 (station) out of 255. Adjust `MAX_BRIGHTNESS` in the source if LEDs are too dim or drawing too much current.
