#pragma once

#include <Wire.h>
#include <Arduino.h>

// ── Minimal QMI8658 accelerometer driver for Waveshare ESP32-S3-Matrix ────────
// I2C pins: SDA=GPIO11, SCL=GPIO12
// Only accelerometer is used (no gyroscope needed for gravity detection).

namespace pulleys {

// QMI8658 I2C address and registers
static constexpr uint8_t QMI8658_ADDR      = 0x6B;
static constexpr uint8_t REG_WHO_AM_I      = 0x00;
static constexpr uint8_t REG_CTRL1         = 0x02;
static constexpr uint8_t REG_CTRL2         = 0x03;
static constexpr uint8_t REG_CTRL5         = 0x06;
static constexpr uint8_t REG_CTRL7         = 0x08;
static constexpr uint8_t REG_RESET         = 0x60;
static constexpr uint8_t REG_RST_RESULT    = 0x4D;
static constexpr uint8_t REG_STATUS0       = 0x2E;
static constexpr uint8_t REG_AX_L          = 0x35;

static constexpr uint8_t WHO_AM_I_VAL      = 0x05;
static constexpr uint8_t RESET_CMD         = 0xB0;
static constexpr uint8_t RST_RESULT_OK     = 0x80;

// Accelerometer range: ±2g  → scale = 2.0/32768.0
static constexpr float ACCEL_SCALE = 2.0f / 32768.0f;

struct AccelData {
    float x;  // g-force
    float y;
    float z;
};

class IMU {
public:
    // Initialize the QMI8658 IMU. Returns true on success.
    bool init(uint8_t sdaPin = 11, uint8_t sclPin = 12) {
        Wire.begin(sdaPin, sclPin, 400000);

        // Check WHO_AM_I
        uint8_t id = readReg(REG_WHO_AM_I);
        if (id != WHO_AM_I_VAL) {
            Serial.printf("  [IMU] WHO_AM_I mismatch: got 0x%02X, expected 0x%02X\n", id, WHO_AM_I_VAL);
            return false;
        }

        // Soft reset
        writeReg(REG_RESET, RESET_CMD);
        delay(20);
        uint8_t rst = readReg(REG_RST_RESULT);
        if (rst != RST_RESULT_OK) {
            Serial.printf("  [IMU] Reset result: 0x%02X (expected 0x%02X)\n", rst, RST_RESULT_OK);
            // Continue anyway — some revisions differ
        }

        // CTRL1: enable address auto-increment (bit 6)
        writeReg(REG_CTRL1, 0x40);

        // CTRL2: accelerometer config
        //   Range = ±2g (bits 6:4 = 000)
        //   ODR   = 62.5 Hz (bits 3:0 = 0101) — plenty for gravity sensing
        writeReg(REG_CTRL2, 0x05);

        // CTRL5: low-pass filter — mode 0 for accel (default is fine)
        writeReg(REG_CTRL5, 0x01);

        // CTRL7: enable accelerometer only (bit 0 = 1, bit 1 = 0)
        writeReg(REG_CTRL7, 0x01);

        delay(10);  // let first samples accumulate

        _initialized = true;
        Serial.println("  [IMU] QMI8658 initialized (accel ±2g @ 62.5Hz)");
        return true;
    }

    // Read accelerometer. Returns true if new data was available.
    bool read(AccelData& out) {
        if (!_initialized) return false;

        // Check data-ready (STATUS0 bit 0 = accel data available)
        uint8_t status = readReg(REG_STATUS0);
        if (!(status & 0x01)) return false;

        // Read 6 bytes: AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H
        uint8_t buf[6];
        readRegs(REG_AX_L, buf, 6);

        int16_t rawX = (int16_t)((buf[1] << 8) | buf[0]);
        int16_t rawY = (int16_t)((buf[3] << 8) | buf[2]);
        int16_t rawZ = (int16_t)((buf[5] << 8) | buf[4]);

        out.x = rawX * ACCEL_SCALE;
        out.y = rawY * ACCEL_SCALE;
        out.z = rawZ * ACCEL_SCALE;

        return true;
    }

    bool initialized() const { return _initialized; }

private:
    bool _initialized = false;

    void writeReg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(QMI8658_ADDR);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }

    uint8_t readReg(uint8_t reg) {
        Wire.beginTransmission(QMI8658_ADDR);
        Wire.write(reg);
        Wire.endTransmission(false);
        Wire.requestFrom(QMI8658_ADDR, (uint8_t)1);
        return Wire.read();
    }

    void readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
        Wire.beginTransmission(QMI8658_ADDR);
        Wire.write(reg);
        Wire.endTransmission(false);
        Wire.requestFrom(QMI8658_ADDR, len);
        for (uint8_t i = 0; i < len; i++) {
            buf[i] = Wire.read();
        }
    }
};

} // namespace pulleys
