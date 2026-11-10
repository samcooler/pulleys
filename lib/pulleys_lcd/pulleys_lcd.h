#pragma once
// ── The 800×480 RGB panel the arbiter boards carry ────────────────────────────
// Pin map, timing and the LovyanGFX device for it, shared by every image that
// drives one: the arbiter (src/arbiter_mesh) and the television screen
// (src/television). The pins and the pixel clock were each found the hard way
// -- see the notes on LCD_FREQ -- so they live once, here.
//
// Two boards:
//   default                    Waveshare ESP32-S3-Touch-LCD-4.3 (CH422G expander)
//   ARBITER_BOARD_CROWPANEL7   Elecrow CrowPanel Advance 7.0" (V1.2+)
//
// The device has no touch instance: LovyanGFX's GT911 driver takes ownership of
// I2C port 0, the bus the RTC and the CH422G expander sit on. Touch is read
// with pulleys_gt911 over Arduino's Wire instead.

// ESP32-S3 RGB panel classes aren't pulled in by LovyanGFX.hpp by default
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <LovyanGFX.hpp>

#if defined(ARBITER_BOARD_CROWPANEL7)
// From Elecrow's LovyanGFX_Driver.h for the Advance 7" (V1.2 through V1.5).
#define LCD_DE    42
#define LCD_VSYNC 41
#define LCD_HSYNC 40
#define LCD_PCLK  39
#define LCD_D0  21   // B0
#define LCD_D1  47   // B1
#define LCD_D2  48   // B2
#define LCD_D3  45   // B3
#define LCD_D4  38   // B4
#define LCD_D5   9   // G0
#define LCD_D6  10   // G1
#define LCD_D7  11   // G2
#define LCD_D8  12   // G3
#define LCD_D9  13   // G4
#define LCD_D10 14   // G5
#define LCD_D11  7   // R0
#define LCD_D12 17   // R1
#define LCD_D13 18   // R2
#define LCD_D14  3   // R3
#define LCD_D15 46   // R4
// Elecrow's examples say 18 MHz, but LovyanGFX divides PCLK from 240 MHz and
// 240/18 needs a fractional divider: uneven pixel clock periods made the whole
// image jitter left and right. 16 MHz (exact 240/15) still shook slightly —
// PSRAM scan-out pressure — and 15 MHz (240/16) is steady.
#define LCD_FREQ       15000000
#define LCD_SYNC_POL   1
#define LCD_PCLK_IDLE  1
#else
// ── Display pin mapping — from board schematic ────────────────────────────────
// Control
#define LCD_DE     5    // Data Enable
#define LCD_VSYNC  3    // Vertical sync
#define LCD_HSYNC 46    // Horizontal sync
#define LCD_PCLK   7    // Pixel clock

// RGB565 data lines mapped to Bus_RGB pin_d0..d15 (d0 = LSB)
// Red bits:   R3=GPIO1, R4=GPIO2, R5=GPIO42, R6=GPIO41, R7=GPIO40
// Green bits: G2=GPIO39, G3=GPIO0, G4=GPIO45, G5=GPIO48, G6=GPIO47, G7=GPIO21
// Blue bits:  B3=GPIO14, B4=GPIO38, B5=GPIO18, B6=GPIO17, B7=GPIO10
//
// d[0..4]   = B[0..4] in RGB565 = board's B3..B7
// d[5..10]  = G[0..5] in RGB565 = board's G2..G7
// d[11..15] = R[0..4] in RGB565 = board's R3..R7
#define LCD_D0  14   // B3
#define LCD_D1  38   // B4
#define LCD_D2  18   // B5
#define LCD_D3  17   // B6
#define LCD_D4  10   // B7
#define LCD_D5  39   // G2
#define LCD_D6   0   // G3
#define LCD_D7  45   // G4
#define LCD_D8  48   // G5
#define LCD_D9  47   // G6
#define LCD_D10 21   // G7
#define LCD_D11  1   // R3
#define LCD_D12  2   // R4
#define LCD_D13 42   // R5
#define LCD_D14 41   // R6
#define LCD_D15 40   // R7
// 16 MHz. Dropping to 12 to ease PSRAM contention blanked the panel to solid
// white: this glass has a minimum pixel clock and loses sync below it, so
// bandwidth has to be won elsewhere.
#define LCD_FREQ       16000000
#define LCD_SYNC_POL   0
#define LCD_PCLK_IDLE  0
#endif

#define LCD_WIDTH    800
#define LCD_HEIGHT   480

namespace pulleys {

class LcdPanel : public lgfx::LGFX_Device {
    lgfx::Panel_RGB   _panel_instance;
    lgfx::Bus_RGB     _bus_instance;

public:
    LcdPanel() {
        { // Bus
            auto cfg = _bus_instance.config();
            cfg.panel             = &_panel_instance;
            cfg.pin_d0            = LCD_D0;
            cfg.pin_d1            = LCD_D1;
            cfg.pin_d2            = LCD_D2;
            cfg.pin_d3            = LCD_D3;
            cfg.pin_d4            = LCD_D4;
            cfg.pin_d5            = LCD_D5;
            cfg.pin_d6            = LCD_D6;
            cfg.pin_d7            = LCD_D7;
            cfg.pin_d8            = LCD_D8;
            cfg.pin_d9            = LCD_D9;
            cfg.pin_d10           = LCD_D10;
            cfg.pin_d11           = LCD_D11;
            cfg.pin_d12           = LCD_D12;
            cfg.pin_d13           = LCD_D13;
            cfg.pin_d14           = LCD_D14;
            cfg.pin_d15           = LCD_D15;
            cfg.pin_henable       = LCD_DE;
            cfg.pin_vsync         = LCD_VSYNC;
            cfg.pin_hsync         = LCD_HSYNC;
            cfg.pin_pclk          = LCD_PCLK;
            cfg.freq_write        = LCD_FREQ;
            cfg.hsync_polarity    = LCD_SYNC_POL;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;
            cfg.vsync_polarity    = LCD_SYNC_POL;
            cfg.vsync_front_porch = 8;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 8;
            cfg.pclk_active_neg   = 1;
            cfg.de_idle_high      = 0;
            cfg.pclk_idle_high    = LCD_PCLK_IDLE;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        { // Panel — framebuffer lives in PSRAM (800×480×2 = 768 KB)
            auto cfg = _panel_instance.config();
            cfg.memory_width  = LCD_WIDTH;
            cfg.memory_height = LCD_HEIGHT;
            cfg.panel_width   = LCD_WIDTH;
            cfg.panel_height  = LCD_HEIGHT;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _panel_instance.config(cfg);

            auto dcfg = _panel_instance.config_detail();
            dcfg.use_psram = 2;   // 0=SRAM, 1=both, 2=PSRAM only
            _panel_instance.config_detail(dcfg);
        }
        setPanel(&_panel_instance);
    }
};

}  // namespace pulleys
