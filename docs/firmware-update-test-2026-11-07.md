# Gauge OTA test — November 7, 2026

## Result

PASS for one complete update at the current room placement. This is not yet a
room-wide reliability or repeated-update soak test.

- Gauge: `59A4`, MAC `B4:BF:E9:0F:93:68`, `component_state_display`.
- Replacement bridge: `1966`, MAC `B4:BF:E9:6A:DA:E8`, classic ESP32,
  4 MB flash, `bridge_esp32`, USB-UART at 115200 baud.
- Coordinator: `overlord.local`, `192.168.1.38` during the test.
- Target: published `v0.1.0`, 977,040 bytes.
- Asset SHA-256: `5440b66acaca9888b458ea2ec8e83fc11e2fd8a01d7b5aee6ea9a8ba2b1b9e74`.
- Build identity: `1d6f8db87630221b5b048e8fc9471b5d23057a23e3e540674dd1f6db09eee3aa`.

The temporary `/tmp/test-gauge-ota.py` harness on the Overlord used the actual
`SerialReader`, `FirmwareUpdater.handle_line`, chunk-serving and post-reboot
inventory methods. It offered the cached, SHA-256-verified release asset only
to `59A4`; it did not substitute a firmware server on the Mac.

Observed bridge log:

```text
[FW] HTTP served 977040/977040 bytes
FW-STATUS id=59A4 env=component_state_display state=ok ... err=0
PULLEYS-CENSUS id=59A4 ... env=component_state_display ... up=9 self=0 sha256=1d6f8db87630221b5b048e8fc9471b5d23057a23e3e540674dd1f6db09eee3aa
```

The gauge verified its image, rebooted and reported the expected build identity.
The normal Overlord service was restored after the targeted test. The bridge
and coordinator fixes are deployed; the gauge now runs published `v0.1.0`,
not the temporary USB diagnostic build used earlier in this session.

## Findings and changes

The original C3 bridge ran an October 18 UART1 image while connected by USB.
It required a USB migration flash. Its replacement is a classic ESP32 and has
its own environment so a C3 firmware image will not be offered to it.

The firmware AP was already open: no password or encryption was removed.
Wi-Fi association failures were intermittent; the replacement bridge connected
on the successful attempt. Antenna performance has not been isolated from the
software changes by a controlled comparison.

Transfers exposed missing chunk responses and retry cascades from late replies.
The bridge now retries bounded requests and ignores replies for other offsets.
Native-USB firmware traffic permits bounded transmit backpressure. HardwareSerial
gets its receive buffer before `begin()`. AP startup is checked and uses four
client slots. The coordinator allows a size-based deadline for USB-UART transfers.

Validation: both bridge environments compiled, the gauge diagnostic firmware
compiled and flashed, manifest loading passed with and without the optional
classic ESP32 asset, and an extracted C++ chunk-parser harness passed stale
response, lost response and bounded timeout cases. Python compilation, shell
syntax and `git diff --check` passed. These observations describe the initial hardware test before publication of
the next firmware bundle.
