# Field Guide

## One-liners

```sh
./go.sh station_wroom
./go.sh traveler

./flash_all.sh station_s3   # flash only (skips build)
./serial_watch.sh           # monitor only, auto-reconnects
```

## Calibrating detection range

Walk a traveler toward the station and watch serial output:
```
[PROX] T-A3F2: FAR → CLOSE (RSSI -55 dBm)
```
Culture exchange triggers at **CLOSE**. If it fires too early (too far away), make `RSSI_CLOSE` more negative. If it never fires, make it less negative.

Edit `platformio.ini`, find the station env you're using, change the threshold:
```ini
-D PULLEYS_RSSI_CLOSE=-58    ← adjust this (default -58, more negative = closer range)
```
Then rebuild: `./go.sh station_s3`

## Other knobs (in source, need rebuild)

| What | File | Variable |
|------|------|----------|
| LED brightness | `src/station/main.cpp` | `MAX_BRIGHTNESS` |
| Culture exchange cooldown | `src/station/main.cpp` | `MATE_COOLDOWN_MS` |
| Traveler sleep timeout | `src/traveler/main.cpp` | `SLEEP_TIMEOUT_MS` |
| Traveler beacon rate | `src/traveler/main.cpp` | `BEACON_INTERVAL_MS` |

## Ports

```sh
ls /dev/cu.usb*          # see what's connected
```
- `usbmodem*` — S3/C3 (native USB)
- `usbserial*` / `wchusbserial*` — WROOM / CH340

## Sanity checks in serial output

Good boot looks like:
```
PULLEYS Station
LEDs ready — pin 10, 256 leds
Scanning for Travelers...
Station ready.
```

Culture exchange sequence:
```
★ T-A3F2 → slot 2: teal/mix 0.64Hz
[ABS] Flash
[ABS] Resolve
[ABS] Restore
[ABS] Done
```
