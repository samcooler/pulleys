#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>

// ── PCF85063 real-time clock ─────────────────────────────────────────────────
//
// Identified on the Waveshare ESP32-S3-Touch-LCD-4.3 at I2C 0x51, confirmed by
// watching register 0x04 tick in BCD. (Seconds at 0x04 is what distinguishes a
// PCF85063 from a PCF8563, which keeps them at 0x02.)
//
// Bit 7 of the seconds register is the oscillator-stop flag: the chip sets it
// whenever it has lost power, meaning the time it reports is meaningless until
// somebody sets it. Always check valid() before trusting a timestamp — an
// event log stamped 2000-01-01 is worse than one honestly marked unsynced.
//
// Like everything else on this bus, it must be reached before LovyanGFX's
// touch driver claims I2C port 0.

namespace pulleys {

static constexpr uint8_t PCF85063_ADDR    = 0x51;
static constexpr uint8_t PCF85063_CTRL1   = 0x00;
static constexpr uint8_t PCF85063_SECONDS = 0x04;   // bit7 = OS (oscillator stopped)

struct RtcTime {
    uint16_t year   = 2000;
    uint8_t  month  = 1;
    uint8_t  day    = 1;
    uint8_t  hour   = 0;
    uint8_t  minute = 0;
    uint8_t  second = 0;
};

class RTC {
public:
    // Wire must already be begun.
    bool begin() {
        Wire.beginTransmission(PCF85063_ADDR);
        _present = (Wire.endTransmission() == 0);
        return _present;
    }

    bool present() const { return _present; }

    // False when the chip has lost power since it was last set, so its time is
    // not to be believed.
    bool valid() {
        uint8_t s;
        if (!readReg(PCF85063_SECONDS, s)) return false;
        return (s & 0x80) == 0;
    }

    bool now(RtcTime& t) {
        uint8_t b[7];
        if (!readRegs(PCF85063_SECONDS, b, 7)) return false;
        t.second = bcd2dec(b[0] & 0x7F);
        t.minute = bcd2dec(b[1] & 0x7F);
        t.hour   = bcd2dec(b[2] & 0x3F);
        t.day    = bcd2dec(b[3] & 0x3F);
        // b[4] is weekday — derivable from the date, so not stored
        t.month  = bcd2dec(b[5] & 0x1F);
        t.year   = 2000 + bcd2dec(b[6]);
        return true;
    }

    // Setting the time also clears the oscillator-stop flag.
    bool set(const RtcTime& t) {
        uint8_t b[7] = {
            (uint8_t)(dec2bcd(t.second) & 0x7F),   // clears OS
            dec2bcd(t.minute),
            dec2bcd(t.hour),
            dec2bcd(t.day),
            (uint8_t)weekday(t.year, t.month, t.day),
            dec2bcd(t.month),
            dec2bcd((uint8_t)(t.year % 100)),
        };
        return writeRegs(PCF85063_SECONDS, b, 7);
    }

    // "2026-08-30 22:58:27" — sorts correctly as text, which matters for a log.
    static void format(const RtcTime& t, char* out, size_t n) {
        snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    }

private:
    bool _present = false;

    static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
    static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

    // Zeller's congruence — the chip stores a weekday but never computes one.
    static uint8_t weekday(uint16_t y, uint8_t m, uint8_t d) {
        if (m < 3) { m += 12; y -= 1; }
        uint16_t k = y % 100, j = y / 100;
        int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        return (uint8_t)((h + 6) % 7);   // 0 = Sunday
    }

    bool readReg(uint8_t reg, uint8_t& out) { return readRegs(reg, &out, 1); }

    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
        Wire.beginTransmission(PCF85063_ADDR);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom(PCF85063_ADDR, len) != len) return false;
        for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
        return true;
    }

    bool writeRegs(uint8_t reg, const uint8_t* buf, uint8_t len) {
        Wire.beginTransmission(PCF85063_ADDR);
        Wire.write(reg);
        for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
        return Wire.endTransmission() == 0;
    }
};

}  // namespace pulleys
