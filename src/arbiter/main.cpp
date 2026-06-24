#include <Arduino.h>
// ESP32-S3 RGB panel classes aren't pulled in by LovyanGFX.hpp by default
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include "ble_scanner.h"
#include "ui/arbiter_ui.h"

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

// Touch (GT911) — RST is on CH422G GPIO expander (handled separately)
#define TOUCH_SDA  8
#define TOUCH_SCL  9
#define TOUCH_INT  4
#define TOUCH_RST (-1)   // controlled by CH422G, skip for now

#define LCD_WIDTH    800
#define LCD_HEIGHT   480
#define LCD_BUF_LINES 10

// ── LovyanGFX display class ───────────────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_RGB   _panel_instance;
    lgfx::Bus_RGB     _bus_instance;
    lgfx::Touch_GT911 _touch_instance;

public:
    LGFX() {
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
            cfg.freq_write        = 16000000;
            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 8;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 8;
            cfg.pclk_active_neg   = 1;
            cfg.de_idle_high      = 0;
            cfg.pclk_idle_high    = 0;
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
        { // Touch (GT911 via I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min  = 0;   cfg.x_max = LCD_WIDTH - 1;
            cfg.y_min  = 0;   cfg.y_max = LCD_HEIGHT - 1;
            cfg.pin_int = TOUCH_INT;
            cfg.pin_rst = TOUCH_RST;
            cfg.bus_shared   = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port = 0;
            cfg.i2c_addr = 0x5D;   // GT911: 0x5D or 0x14
            cfg.pin_sda  = TOUCH_SDA;
            cfg.pin_scl  = TOUCH_SCL;
            cfg.freq     = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

static LGFX display;

// ── LVGL glue ─────────────────────────────────────────────────────────────────
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf1[LCD_WIDTH * LCD_BUF_LINES];

static void lvgl_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    display.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t*)color_p);
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_read(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    uint16_t tx, ty;
    if (display.getTouch(&tx, &ty)) {
        data->point.x = tx;
        data->point.y = ty;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n── Arbiter boot ──");
    Serial.printf("  PSRAM: %u KB total  %u KB free\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);

    bool ok = display.init();
    Serial.printf("  display.init(): %s  (%dx%d)\n",
                  ok ? "OK" : "FAILED", display.width(), display.height());

    if (!ok) {
        Serial.println("  Halting — check PSRAM config and pin mapping");
        while (true) delay(1000);
    }

    display.setRotation(0);

    // ── Direct hardware test — see color flashes before LVGL starts ───────────
    // Red → Green → Blue means panel DMA is working.
    // All-black means pins/timing or PSRAM framebuffer issue.
    display.fillScreen(display.color565(255, 0, 0));
    delay(600);
    display.fillScreen(display.color565(0, 255, 0));
    delay(600);
    display.fillScreen(display.color565(0, 0, 255));
    delay(600);
    display.fillScreen(0);

    // ── LVGL init ─────────────────────────────────────────────────────────────
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, LCD_WIDTH * LCD_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_WIDTH;
    disp_drv.ver_res  = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);

    scanner_init();
    ui_init();
    Serial.println("Arbiter ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t last_scan = 0;
    static uint32_t last_ui   = 0;
    uint32_t now = millis();

    if (now - last_scan >= 1000) {
        last_scan = now;
        scanner_tick();
    }

    ui_animate();
    lv_timer_handler();

    // Refresh UI: immediately after a tap, or every 500ms for live RSSI updates
    if (ui_needs_refresh() || (now - last_ui >= 500)) {
        last_ui = now;
        ui_refresh();
    }

    delay(5);
}
