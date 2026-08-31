#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>

// ── GT911 capacitive touch controller ────────────────────────────────────────
//
// Deliberately driven over Arduino Wire rather than through LovyanGFX's touch
// support. LovyanGFX's driver takes ownership of I2C port 0, which is the same
// bus as the PCF85063 RTC and the CH422G expander that selects the SD card —
// with it enabled, the clock can be read once at boot and never again. One bus
// owner, and everything on the bus keeps working.
//
// Polled, not interrupt-driven: LVGL asks for the pointer state on its own
// schedule, so there is nothing for an interrupt to make more responsive.

namespace pulleys {

static constexpr uint8_t  GT911_ADDR        = 0x5D;   // 0x14 on some boards
static constexpr uint16_t GT911_REG_STATUS  = 0x814E; // bit7 ready, low nibble = points
static constexpr uint16_t GT911_REG_POINT1  = 0x8150;
static constexpr uint16_t GT911_REG_PROD_ID = 0x8140;

class GT911 {
public:
    // Panel size, used to reject impossible coordinates.
    void setBounds(uint16_t w, uint16_t h) { _w = w; _h = h; }

    // Trace every contact from inside the read path. A separate polling dump
    // would clear the controller's ready flag and swallow the very touches the
    // UI is waiting for, so tracing has to happen where the reading happens.
    void setDebug(bool on) { _debug = on; }

    // Wire must already be begun.
    bool begin(uint8_t addr = GT911_ADDR) {
        _addr = addr;
        uint8_t id[4] = {0};
        if (!readRegs(GT911_REG_PROD_ID, id, 4)) return false;
        _present = true;
        Serial.printf("  [TOUCH] GT911 at 0x%02X, product '%c%c%c%c'\n",
                      _addr, id[0], id[1], id[2], id[3]);
        return true;
    }

    bool present() const { return _present; }

    // Returns true while a finger is down, with the first contact's position.
    bool read(uint16_t& x, uint16_t& y) {
        if (!_present) return false;

        uint8_t status;
        if (!readRegs(GT911_REG_STATUS, &status, 1)) return false;
        if (!(status & 0x80)) return false;          // no new data ready

        uint8_t points = status & 0x0F;
        bool touched = false;
        if (points > 0) {
            uint8_t p[8];
            if (readRegs(GT911_REG_POINT1, p, 8)) {
                if (_debug)
                    Serial.printf("  [TOUCH] status=0x%02X pts=%d raw=[%02X %02X %02X %02X] -> x=%u y=%u\n",
                                  status, points, p[0], p[1], p[2], p[3],
                                  (unsigned)(p[0] | (p[1] << 8)),
                                  (unsigned)(p[2] | (p[3] << 8)));
                // 0x8150 is X-low directly. The contact's track id sits at
                // 0x814F, one below — assuming it led the block shifted every
                // coordinate a byte and turned real touches into nonsense.
                uint16_t px = (uint16_t)(p[0] | (p[1] << 8));
                uint16_t py = (uint16_t)(p[2] | (p[3] << 8));
                // A garbled read yields huge values that wrap negative once
                // LVGL casts them to a signed coordinate, so reject anything
                // off-panel rather than passing it on.
                if (px < _w && py < _h) {
                    x = px; y = py;
                    _lastX = x; _lastY = y;
                    _haveLast = true;
                    touched = true;
                }
            }
        }

        // The controller holds the buffer until this flag is cleared; skip it
        // and the same contact is reported forever.
        writeReg(GT911_REG_STATUS, 0);

        if (touched) { _down = true; }
        else         { _down = false; }
        return touched;
    }

    // LVGL polls faster than the panel produces frames, and a poll that finds
    // no fresh data must not be read as a release. Holds the last state until
    // the controller actually reports a change.
    bool readSticky(uint16_t& x, uint16_t& y) {
        uint16_t nx, ny;
        if (read(nx, ny)) { x = nx; y = ny; return true; }
        if (_down && _haveLast) { x = _lastX; y = _lastY; return true; }
        return false;
    }

    // Unfiltered view for debugging: the status byte plus the first contact's
    // raw bytes, with nothing rejected. Returns false only if I2C itself fails.
    bool rawRead(uint8_t& status, uint8_t* pts, uint8_t len, bool clear = true) {
        if (!readRegs(GT911_REG_STATUS, &status, 1)) return false;
        if (len) {
            if (!readRegs(GT911_REG_POINT1, pts, len)) return false;
        }
        if (clear && (status & 0x80)) writeReg(GT911_REG_STATUS, 0);
        return true;
    }

    // Read the controller's configured coordinate range. If this does not match
    // the panel, every real touch lands outside the bounds check and vanishes.
    bool readResolution(uint16_t& w, uint16_t& h) {
        uint8_t b[4];
        if (!readRegs(0x8048, b, 4)) return false;   // config: X/Y output max
        w = (uint16_t)(b[0] | (b[1] << 8));
        h = (uint16_t)(b[2] | (b[3] << 8));
        return true;
    }

    uint8_t address() const { return _addr; }

private:
    uint8_t  _addr    = GT911_ADDR;
    bool     _present = false;
    bool     _debug    = false;
    bool     _down     = false;
    bool     _haveLast = false;
    uint16_t _lastX = 0, _lastY = 0;
    uint16_t _w = 800, _h = 480;

    bool readRegs(uint16_t reg, uint8_t* buf, uint8_t len) {
        Wire.beginTransmission(_addr);
        Wire.write((uint8_t)(reg >> 8));
        Wire.write((uint8_t)(reg & 0xFF));
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom(_addr, len) != len) return false;
        for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
        return true;
    }

    bool writeReg(uint16_t reg, uint8_t val) {
        Wire.beginTransmission(_addr);
        Wire.write((uint8_t)(reg >> 8));
        Wire.write((uint8_t)(reg & 0xFF));
        Wire.write(val);
        return Wire.endTransmission() == 0;
    }
};

}  // namespace pulleys
