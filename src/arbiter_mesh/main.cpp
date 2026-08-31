#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <pulleys_ch422g.h>
#include <pulleys_rtc.h>
#include "eventlog.h"
#include "clocksrc.h"
#include "clockui.h"
#include <pulleys_gt911.h>
// ESP32-S3 RGB panel classes aren't pulled in by LovyanGFX.hpp by default
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <pulleys_identity.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include "monitor.h"
#include "ui.h"

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

// ── microSD (SPI; chip-select lives on the CH422G, not a GPIO) ────────────────
#define SD_MOSI  11
#define SD_SCK   12
#define SD_MISO  13
// The SD library insists on driving a GPIO for chip-select, but the real select
// is EXIO4 on the expander. Since the card is the only device on this bus we
// hold EXIO4 asserted and hand the library an unconnected pin to waggle. A card
// enters SPI mode by seeing CS low during CMD0, so a permanently asserted
// select is what the card wants anyway.
#define SD_CS_DUMMY 15

#define LCD_WIDTH    800
#define LCD_HEIGHT   480
#define LCD_BUF_LINES 40   // fewer, larger flushes into the live framebuffer

// ── LovyanGFX display class ───────────────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_RGB   _panel_instance;
    lgfx::Bus_RGB     _bus_instance;
    // No touch instance: this display is read-only, and LovyanGFX's GT911
    // driver takes ownership of I2C port 0 — the bus the RTC and the CH422G
    // expander sit on. Keeping the bus means the clock stays readable and
    // settable while running, which the event log depends on.

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
            // 12 MHz rather than 16: the panel DMA and the CPU share PSRAM
            // bandwidth, and underruns show up as shimmer on thin strokes.
            // 800x480 plus porches at 12 MHz still gives ~29 fps.
            cfg.freq_write        = 12000000;
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
        setPanel(&_panel_instance);
    }
};

static LGFX display;
static pulleys::RTC   s_rtc;
static pulleys::GT911 s_touch;
static bool           s_touchDebug = false;

// Serial console: T<YYYYMMDDHHMMSS> sets the clock. Fixed width so there is no
// ambiguity about field order, and local wall time rather than epoch because
// time-of-day is the question the log exists to answer.
static void handleSerial() {
    static char buf[32];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            uint8_t n = len;
            len = 0;
            if (buf[0] == 'T' && n == 15) {
                auto num = [&](uint8_t off, uint8_t w) {
                    int v = 0;
                    for (uint8_t i = 0; i < w; i++) v = v * 10 + (buf[1 + off + i] - '0');
                    return v;
                };
                pulleys::RtcTime t;
                t.year   = num(0, 4); t.month  = num(4, 2); t.day    = num(6, 2);
                t.hour   = num(8, 2); t.minute = num(10, 2); t.second = num(12, 2);
                if (t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
                    t.hour < 24 && t.minute < 60 && t.second < 60 && s_rtc.set(t)) {
                    clocksrc_mark_set();
                    char o[24]; pulleys::RTC::format(t, o, sizeof(o));
                    Serial.printf("  [RTC] set to %s (source now: %s)\n",
                                  o, clocksrc_source_str());
                } else {
                    Serial.println("  [RTC] set FAILED — expected T<YYYYMMDDHHMMSS>");
                }
            } else if (buf[0] == 'X') {
                s_touchDebug = !s_touchDebug;
                s_touch.setDebug(s_touchDebug);
                Serial.printf("  [TOUCH] trace %s\n", s_touchDebug ? "ON" : "off");
                uint16_t rw, rh;
                if (s_touch.readResolution(rw, rh))
                    Serial.printf("  [TOUCH] controller resolution %ux%u (panel %dx%d)\n",
                                  rw, rh, LCD_WIDTH, LCD_HEIGHT);
                else
                    Serial.println("  [TOUCH] could not read controller resolution");
            } else if (buf[0] == 'D') {
                eventlog_dump();
            } else if (buf[0] == 'L') {
                Serial.printf("  [LOG] %s  file=%s rows=%lu dropped=%lu\n",
                              eventlog_ok() ? "ok" : "STOPPED", eventlog_path(),
                              (unsigned long)eventlog_rows(),
                              (unsigned long)eventlog_dropped());
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ── LVGL glue ─────────────────────────────────────────────────────────────────
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf1[LCD_WIDTH * LCD_BUF_LINES];

static void lvgl_touch_read(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    uint16_t tx, ty;
    if (s_touch.readSticky(tx, ty)) {
        data->point.x = (lv_coord_t)tx;
        data->point.y = (lv_coord_t)ty;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lvgl_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    display.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t*)color_p);
    lv_disp_flush_ready(drv);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n── Arbiter (mesh monitor) boot ──");
    Serial.printf("  PSRAM: %u KB total  %u KB free\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);

    // ── Bus probe ─────────────────────────────────────────────────────────────
    // Reports what is actually on the I2C bus before trusting any pin map from
    // memory. The GT911 touch controller and the CH422G expander share it, and
    // on this board family the microSD chip-select hangs off that expander.
    {
        Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
        Serial.print("  [I2C] devices:");
        int found = 0;
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
        }
        Serial.println(found ? "" : " none");
    }

    // ── Real-time clock ───────────────────────────────────────────────────────
    // Confirmed a PCF85063 by watching register 0x04 tick in BCD. Seconds at
    // 0x04 rather than 0x02 is what tells it apart from a PCF8563.
    s_rtc.begin();
    clocksrc_init(&s_rtc);

    s_touch.setBounds(LCD_WIDTH, LCD_HEIGHT);
    if (!s_touch.begin()) Serial.println("  [TOUCH] GT911 not found — clock UI unusable");

    // ── microSD ───────────────────────────────────────────────────────────────
    // Must run before display.init(): LovyanGFX's GT911 driver takes over I2C
    // port 0, after which Arduino's Wire can no longer reach the expander.
    {
        static pulleys::CH422G expander;
        if (!expander.init()) {
            Serial.println("  [SD] CH422G not responding — no card access");
        } else {
            // Prove the expander actually drives its pins before blaming the
            // card: a write can be ACKed while the outputs stay high-impedance.
            uint8_t lo = 0, hi = 0;
            if (expander.selfTest(pulleys::CH422G_EXIO_SD_CS, lo, hi)) {
                Serial.printf("  [SD] EXIO readback low=0x%02X high=0x%02X  (bit4: %d -> %d)\n",
                              lo, hi, (lo >> 4) & 1, (hi >> 4) & 1);
                if (((lo >> 4) & 1) == ((hi >> 4) & 1))
                    Serial.println("       EXIO4 is NOT moving — expander outputs not enabled");
            } else {
                Serial.println("  [SD] EXIO readback unavailable");
            }

            pinMode(SD_MISO, INPUT_PULLUP);   // SPI-mode SD needs MISO pulled up
            SPI.begin(SD_SCK, SD_MISO, SD_MOSI);

            // Some cards want to see chip-select released once before the first
            // command, so cycle it rather than only ever asserting.
            expander.set(pulleys::CH422G_EXIO_SD_CS, true);
            delay(10);
            expander.set(pulleys::CH422G_EXIO_SD_CS, false);   // active low
            delay(10);

            // Walk the clock down: a long ribbon to the card slot will not hold
            // 20 MHz, and the failure looks identical to an absent card.
            const uint32_t speeds[] = { 20000000, 10000000, 4000000, 1000000, 400000 };
            bool mounted = false;
            for (uint8_t i = 0; i < 5 && !mounted; i++) {
                if (SD.begin(SD_CS_DUMMY, SPI, speeds[i])) {
                    Serial.printf("  [SD] mounted at %lu kHz\n",
                                  (unsigned long)(speeds[i] / 1000));
                    mounted = true;
                } else {
                    SD.end();
                    delay(20);
                }
            }

            if (!mounted) {
                Serial.println("  [SD] mount FAILED at every speed.");
                Serial.println("       card inserted? formatted FAT32 (not exFAT)?");
            } else {
                const char* kind = "?";
                switch (SD.cardType()) {
                    case CARD_MMC:  kind = "MMC";   break;
                    case CARD_SD:   kind = "SDSC";  break;
                    case CARD_SDHC: kind = "SDHC";  break;
                    case CARD_NONE: kind = "none";  break;
                    default: break;
                }
                Serial.printf("  [SD] %s, %llu MB total, %llu MB used\n",
                              kind, SD.cardSize() / (1024ULL * 1024ULL),
                              SD.usedBytes() / (1024ULL * 1024ULL));

                // Prove it round-trips before trusting it with an event log.
                File f = SD.open("/pulleys_probe.txt", FILE_WRITE);
                if (!f) {
                    Serial.println("  [SD] open for write FAILED");
                } else {
                    f.printf("pulleys arbiter probe, millis=%lu\n", (unsigned long)millis());
                    f.close();
                    File r = SD.open("/pulleys_probe.txt", FILE_READ);
                    if (r) {
                        Serial.printf("  [SD] read back OK: %s", r.readString().c_str());
                        r.close();
                    } else {
                        Serial.println("  [SD] read back FAILED");
                    }
                }
            }
        }
    }

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

    // Join the mesh as an observer. Relaying is on by default — a monitor that
    // relays is a legitimate extra hop — but MESH_OBSERVE_ONLY makes it passive
    // so it cannot paper over the range gaps you are hunting for.
    pulleys::identity_init(PULLEYS_TYPE_ARBITER);
    pulleys::mesh_init(pulleys::MESH_ORIGIN_ARBITER, pulleys::identity_id());
#ifdef MESH_OBSERVE_ONLY
    pulleys::mesh_set_relay(false);
    Serial.println("  [MESH] observe-only: relaying disabled");
#endif
    monitor_init();
    eventlog_init(&s_rtc);
    monitor_on_event(eventlog_record);
    ui_init();
    clockui_init(&s_rtc);
    Serial.printf("Arbiter (mesh monitor) ready — %s\n", pulleys::identity_name());
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t last_tick = 0;
    static uint32_t last_ui   = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    handleSerial();
    eventlog_tick();
    clocksrc_tick();

    if (now - last_tick >= 1000) {
        last_tick = now;
        monitor_tick();
    }

    clockui_tick();
    lv_timer_handler();

    if (now - last_ui >= 750 && !clockui_is_open()) {
        last_ui = now;
        ui_refresh();
    }

    // Mirror the panel to serial. A monitor you can only read by standing in
    // front of it is no use when the panel itself is what you are debugging.
    static uint32_t last_dump = 0;
    if (now - last_dump >= 4000) {
        last_dump = now;
        const MonNode* nodes = monitor_nodes();
        Serial.printf("── nodes: %d sensor / %d screen | events %lu (%.0f/min) | skew spread %ldms | clock %s\n",
                      monitor_count(pulleys::MESH_ORIGIN_SENSOR),
                      monitor_count(pulleys::MESH_ORIGIN_SCREEN),
                      (unsigned long)monitor_total_events(),
                      monitor_events_per_min(),
                      (long)monitor_skew_spread(),
                      pulleys::mesh_clock_locked() ? "locked" : "free");
        Serial.printf("   log %s rows=%lu dropped=%lu clock=%s\n",
                      eventlog_ok() ? "ok" : "STOPPED",
                      (unsigned long)eventlog_rows(),
                      (unsigned long)eventlog_dropped(),
                      clocksrc_source_str());
        for (uint8_t i = 0; i < MON_MAX_NODES; i++) {
            const MonNode& n = nodes[i];
            if (!n.active) continue;
            uint32_t seenS = (now - n.lastBeaconMs) / 1000;
            if (n.type == pulleys::MESH_ORIGIN_SENSOR) {
                char chS[6], modeS[4];
                if (n.events) {
                    snprintf(chS, sizeof(chS), "ch%-2d", n.channel);
                    snprintf(modeS, sizeof(modeS), "%s",
                             n.mode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin");
                } else {
                    snprintf(chS, sizeof(chS), "ch??");   // not known until an event
                    snprintf(modeS, sizeof(modeS), "?  ");
                }
                Serial.printf("   SENSOR  %04X %s %s ev=%lu skew=%+ldms seen=%lus\n",
                              n.id, chS, modeS,
                              (unsigned long)n.events, (long)n.skewMs,
                              (unsigned long)seenS);
            } else {
                // Channel/mode/events are sensor-only; printing them for a
                // screen invents data that does not exist.
                Serial.printf("   %s %04X skew=%+ldms seen=%lus\n",
                              n.type == pulleys::MESH_ORIGIN_SCREEN ? "SCREEN " : "ARBITER",
                              n.id, (long)n.skewMs, (unsigned long)seenS);
            }
        }
    }

    delay(5);
}
