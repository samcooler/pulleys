# Developing Pulleys

## Hardware

| Role | Board | LEDs | Extras |
|------|-------|------|--------|
| **Traveler** | Waveshare ESP32-S3-Matrix | 8×8 WS2812B matrix, 64 LEDs (GPIO 14) | QMI8658 IMU, battery ADC (GPIO 2) |
| **Station** | Seeed XIAO ESP32-C3 | 8×32 WS2812B matrix, 256 LEDs (GPIO 10) | — |

## Build Environments

```
pio run -e traveler      # build traveler firmware
pio run -e station       # build station for XIAO ESP32-C3
```

Default `pio run` builds `traveler` + `station`.

Note: PlatformIO CLI is at `~/.platformio/penv/bin/pio` if not on your PATH.

## Flashing & Monitoring

One board at a time via USB-C. The PlatformIO device monitor locks the serial port, so **close it before flashing**:

```bash
# Kill any running monitor first
pkill -f "platformio device monitor"

# Flash traveler
pio run -e traveler -t upload

# Monitor traveler serial output
pio device monitor -e traveler

# Flash + monitor in one shot
pio run -e traveler -t upload && pio device monitor -e traveler
```

Same for station:
```bash
pkill -f "platformio device monitor"
pio run -e station -t upload && pio device monitor -e station
```

### Parallel Flashing

`flash_all.sh` flashes all connected boards in parallel via `esptool.py`. It auto-detects chip type from the environment name (`station*` → esp32c3, else → esp32s3) and finds all `/dev/cu.usbmodem*` ports. Requires a prior `pio run` build.

```bash
./flash_all.sh traveler    # flash all connected boards with traveler firmware
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
│   └── station/main.cpp    # Station firmware
├── flash_all.sh            # Parallel flash script
├── serial_watch.sh         # Auto-reconnecting serial monitor
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
- **Boot color preview** — the traveler shows its two culture colors as two solid rows in the center of the matrix for 1 second on boot, for visual verification before the pattern starts.
- **Proximity zones** — serial logs zone transitions (FAR → NEAR → CLOSE → GONE). Station prints a summary of all tracked travelers every 2 seconds.
- **Mating cooldown** — a station can only absorb culture from the same traveler once every 30 seconds, to prevent over-blending from a single visitor.
- **TX power** — traveler broadcasts at -6 dBm (reduced from default). This tightens proximity zones and saves battery. Changing TX power requires recalibrating RSSI thresholds via walk tests.
- **FastLED brightness** — capped at 32 (traveler) / 15 (station) out of 255. Lower values improve color saturation on dense LED matrices. Adjust `MAX_BRIGHTNESS` in the source if needed.
- **Threshold tuning** — current RSSI thresholds (CLOSE -58, NEAR -73, FAR -80) were calibrated by walk tests at -6 dBm TX power. If you change TX power or antenna environment, re-tune by walking at a known constant speed and adjusting values in `pulleys_proximity.h`.
- **Sleep/wake** — travelers enter light sleep after 30s of no motion. USB serial disconnects during sleep (ESP32-S3 CDC limitation). Use `serial_watch.sh` for auto-reconnecting serial output.
- **Debug flashes** — set `DEBUG_FLASH = true` in traveler `main.cpp` to enable color-coded LED flashes for sleep/wake debugging (red=sleep entry, green=GPIO wake, blue=timer wake, yellow=false trigger, white=exit sleep loop).
- **Battery indicator** — when a traveler enters sleep, it flashes 0–4 dim red LEDs showing battery level (0 = >80%, 4 = <20%). This always shows regardless of DEBUG_FLASH.
- **Dream state** — sleeping travelers periodically wake for a 2-second LED "dream" pattern, then return to sleep. BLE stays off during dreams. Dream interval is ~30s with random jitter.
- **Station 4-slot display** — the station's 8×32 matrix runs 4 independent 8×8 culture slots, each showing the culture of a recent visitor with a multi-phase fade transition.
