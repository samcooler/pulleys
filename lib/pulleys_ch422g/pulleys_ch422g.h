#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>

// ── CH422G I2C GPIO expander ─────────────────────────────────────────────────
//
// On the Waveshare ESP32-S3-Touch-LCD-4.3 this expander owns the lines the SoC
// has run out of pins for, including the microSD chip-select:
//
//   EXIO1  TP_RST     touch reset      (active low)
//   EXIO2  DISP       backlight enable
//   EXIO3  LCD_RST    panel reset      (active low)
//   EXIO4  SD_CS      microSD select   (active low)
//   EXIO5  USB_SEL    USB / CAN mux
//
// The part is addressed unusually: the I2C *address* is the command and the
// payload is a single byte. That is why it answers across a whole block of
// addresses in a bus scan rather than at one address like a normal device.
//
// There is no way to read the output latch back, so the driver keeps a shadow.
// It starts all-high — resets released, backlight on, chip-select idle — which
// matches the power-on state the panel is already running in.

namespace pulleys {

static constexpr uint8_t CH422G_CMD_MODE   = 0x24;  // system setting
static constexpr uint8_t CH422G_CMD_IO_OUT = 0x38;  // EXIO0..7 output latch
static constexpr uint8_t CH422G_CMD_IO_IN  = 0x26;  // EXIO input read
static constexpr uint8_t CH422G_MODE_IO_OE = 0x01;  // drive EXIO as outputs

class CH422G {
public:
    // Wire must already be begun — the touch controller shares this bus.
    bool init() {
        _shadow = 0xFF;
        if (!writeCmd(CH422G_CMD_MODE, CH422G_MODE_IO_OE)) return false;
        return writeCmd(CH422G_CMD_IO_OUT, _shadow);
    }

    // Set one EXIO line. Other lines keep their shadowed state.
    bool set(uint8_t exio, bool high) {
        if (exio > 7) return false;
        uint8_t next = high ? (_shadow | (1 << exio)) : (_shadow & ~(1 << exio));
        if (next == _shadow) return true;
        _shadow = next;
        return writeCmd(CH422G_CMD_IO_OUT, _shadow);
    }

    bool get(uint8_t exio) const { return (_shadow >> exio) & 1; }
    uint8_t shadow() const { return _shadow; }

    // Read the EXIO pin states back. Used to prove the outputs are actually
    // being driven — if a write is ACKed but the pins never move, the mode
    // register is wrong and every downstream symptom looks like dead hardware.
    bool readPins(uint8_t& out) {
        if (Wire.requestFrom((uint8_t)CH422G_CMD_IO_IN, (uint8_t)1) != 1) return false;
        out = Wire.read();
        return true;
    }

    // Drive one line both ways and confirm the readback follows.
    bool selfTest(uint8_t exio, uint8_t& lowRead, uint8_t& highRead) {
        bool okA = set(exio, false); delay(5);
        if (!readPins(lowRead)) return false;
        bool okB = set(exio, true); delay(5);
        if (!readPins(highRead)) return false;
        return okA && okB;
    }

private:
    uint8_t _shadow = 0xFF;

    static bool writeCmd(uint8_t cmd, uint8_t value) {
        Wire.beginTransmission(cmd);
        Wire.write(value);
        return Wire.endTransmission() == 0;
    }
};

// Board line assignments
static constexpr uint8_t CH422G_EXIO_TP_RST  = 1;
static constexpr uint8_t CH422G_EXIO_DISP    = 2;
static constexpr uint8_t CH422G_EXIO_LCD_RST = 3;
static constexpr uint8_t CH422G_EXIO_SD_CS   = 4;
static constexpr uint8_t CH422G_EXIO_USB_SEL = 5;

}  // namespace pulleys
