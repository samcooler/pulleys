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
static constexpr uint8_t REG_CTRL9         = 0x0A;
static constexpr uint8_t REG_CAL1_L        = 0x0B;
static constexpr uint8_t REG_CAL1_H        = 0x0C;
static constexpr uint8_t REG_RESET         = 0x60;
static constexpr uint8_t REG_RST_RESULT    = 0x4D;
static constexpr uint8_t REG_STATUS_INT    = 0x2D;
static constexpr uint8_t REG_STATUS0       = 0x2E;
static constexpr uint8_t REG_STATUS1       = 0x2F;
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
        //   ODR   = 62.5 Hz (bits 3:0 = 0111) — matches 30 FPS LED updates
        writeReg(REG_CTRL2, 0x07);

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

    // Configure Wake-on-Motion mode with CTRL9 command (hardware INT1).
    // threshold: sensitivity 0-255, lower = more sensitive. Default 32.
    // Returns true on success.
    bool configWakeOnMotion(uint8_t threshold = 32) {
        // Soft reset for clean state
        writeReg(REG_RESET, RESET_CMD);
        delay(30);
        for (int i = 0; i < 50; i++) {
            if (readReg(REG_RST_RESULT) == RST_RESULT_OK) break;
            delay(10);
        }

        // Disable all sensors
        writeReg(REG_CTRL7, 0x00);
        delay(10);

        // CTRL1: auto-increment (bit6) + INT1 output enable (bit3)
        writeReg(REG_CTRL1, 0x48);

        // CTRL2: accel ±2g (000), low-power 3Hz ODR (0b1111) — 8.5% duty cycle
        writeReg(REG_CTRL2, 0x0F);

        // CAL1_L: WoM threshold (1mg/LSB resolution per datasheet)
        writeReg(REG_CAL1_L, threshold);

        // CAL1_H: INT1 (bit7:6 = 10), initial HIGH, 4-sample blanking (~1.3s @ 3Hz)
        writeReg(REG_CAL1_H, 0x84);

        // Dump pre-CTRL9 state
        Serial.printf("  [IMU] Pre-CTRL9: CTRL1=0x%02X CTRL7=0x%02X CTRL8=0x%02X CAL1=0x%02X%02X\n",
                      readReg(REG_CTRL1), readReg(0x08), readReg(0x09),
                      readReg(REG_CAL1_H), readReg(REG_CAL1_L));

        // Issue CTRL9 command to activate WoM
        if (!writeCtrl9Cmd(0x08)) {
            Serial.println("  [IMU] CTRL9 WoM command FAILED");
            return false;
        }
        Serial.println("  [IMU] CTRL9 WoM command OK");

        // Check INT1 BEFORE enabling accel
        Serial.printf("  [IMU] Post-CTRL9 (accel off): INT1=%d CTRL8=0x%02X STATUS1=0x%02X\n",
                      gpio_get_level(GPIO_NUM_10), readReg(0x09), readReg(REG_STATUS1));

        // Enable accelerometer
        writeReg(REG_CTRL7, 0x01);
        delay(20);

        // Check INT1 AFTER enabling accel
        Serial.printf("  [IMU] Post-enable: INT1=%d STATUS1=0x%02X\n",
                      gpio_get_level(GPIO_NUM_10), readReg(REG_STATUS1));

        _initialized = false;
        return true;
    }

    // Check if WoM event has occurred by reading STATUS1 register bit 2
    bool checkWomEvent() {
        uint8_t status = readReg(REG_STATUS1);
        return (status & 0x04) != 0;
    }

    // Properly exit WoM mode per datasheet section 9.6, then restore normal accel.
    bool restoreNormalMode(uint8_t sdaPin = 11, uint8_t sclPin = 12) {
        // 1. Disable all sensors
        writeReg(REG_CTRL7, 0x00);
        delay(10);

        // 2. Write threshold = 0 to disable WoM
        writeReg(REG_CAL1_L, 0x00);

        // 3. Execute CTRL9 WoM command with threshold=0 to release INT pins
        writeCtrl9Cmd(0x08);
        delay(10);

        // 4. Now re-init normally
        return init(sdaPin, sclPin);
    }

    // Expose register read for diagnostics
    uint8_t readReg(uint8_t reg) {
        Wire.beginTransmission(QMI8658_ADDR);
        Wire.write(reg);
        Wire.endTransmission(false);
        Wire.requestFrom(QMI8658_ADDR, (uint8_t)1);
        return Wire.read();
    }

private:
    bool _initialized = false;

    void writeReg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(QMI8658_ADDR);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
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

    // CTRL9 protocol: write command, wait for CmdDone, acknowledge
    bool writeCtrl9Cmd(uint8_t cmd) {
        writeReg(REG_CTRL9, cmd);
        // Wait for STATUS_INT bit 7 (CmdDone)
        uint32_t start = millis();
        while (!(readReg(REG_STATUS_INT) & 0x80)) {
            if (millis() - start > 1000) {
                Serial.println("  [IMU] CTRL9 cmd timeout");
                return false;
            }
            delay(1);
        }
        // Acknowledge
        writeReg(REG_CTRL9, 0x00);
        start = millis();
        while (readReg(REG_STATUS_INT) & 0x80) {
            if (millis() - start > 1000) return false;
            delay(1);
        }
        return true;
    }
};

} // namespace pulleys
