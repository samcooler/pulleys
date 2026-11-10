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
#include <pulleys_lcd.h>
#include <lvgl.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include "monitor.h"
#include "gameconfig.h"
#include "gamestate.h"
#include "components.h"
#include "devices.h"
#include "installcfg.h"
#include "installation.h"
#include "installmap.h"
#include "installui.h"
#include "ui.h"
#include "signui.h"

// Two boards run this firmware:
//   default                    Waveshare ESP32-S3-Touch-LCD-4.3 (CH422G expander,
//                              PCF85063 RTC, microSD behind the expander)
//   ARBITER_BOARD_CROWPANEL7   Elecrow CrowPanel Advance 7.0" (V1.2+): an STC8
//                              microcontroller at I2C 0x30 owns the backlight
//                              and touch power; BM8563 RTC; SD not used.
// The panel itself -- pins, pixel clock, the LovyanGFX device -- is
// lib/pulleys_lcd, shared with the television screen.
#if defined(ARBITER_BOARD_CROWPANEL7)
#define TOUCH_SDA 15
#define TOUCH_SCL 16

// The STC8 takes a single command byte: 0 = full backlight ... 245 = off,
// 250 = power up the touch controller.
#define PANEL_MCU_ADDR     0x30
#define PANEL_MCU_TOUCH_ON 250
#ifndef ARBITER_BACKLIGHT
#define ARBITER_BACKLIGHT  0
#endif
#else
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
#endif

#define LCD_BUF_LINES 40   // fewer, larger flushes into the live framebuffer

// No touch instance on the display: LovyanGFX's GT911 driver takes ownership
// of I2C port 0 — the bus the RTC and the CH422G expander sit on. Keeping the
// bus means the clock stays readable and settable while running, which the
// event log depends on.
static pulleys::LcdPanel display;
static pulleys::RTC   s_rtc;
static pulleys::GT911 s_touch;
static bool           s_touchDebug = false;

// Serial console: Q swaps the queue sign and the monitor (signui.h).
// T<YYYYMMDDHHMMSS> sets the clock. Fixed width so there is no ambiguity about
// field order, and local wall time rather than epoch because time-of-day is
// the question the log exists to answer. V lists every device
// setting, wanted against reported; "g diorama.door_opened" injects a device's
// input the way "g <event>" injects a game event.
static void handleSerial() {
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            uint8_t n = len;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;   // "?" → PULLEYS-ID line
            if (pulleys::census_handle_serial(buf)) continue; // "CENSUS" → who is out there
            if (installcfg_handle_serial(buf)) continue;   // MAP / COLOR / SHUFFLE
            if (buf[0] == 'T' && n == 15) {
                auto num = [&](uint8_t off, uint8_t w) {
                    int v = 0;
                    for (uint8_t i = 0; i < w; i++) v = v * 10 + (buf[1 + off + i] - '0');
                    return v;
                };
                pulleys::RtcTime t;
                t.year   = num(0, 4); t.month  = num(4, 2); t.day    = num(6, 2);
                t.hour   = num(8, 2); t.minute = num(10, 2); t.second = num(12, 2);
                bool digits = true;
                for (uint8_t i = 1; i < n; ++i) if (buf[i] < '0' || buf[i] > '9') digits = false;
                if (digits && clocksrc_set(t)) {
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
            } else if (buf[0] == 'p' && n > 1) {
                // "p10" -> a pull on rope channel 10, as if the sensor had sent it
                pulleys::MeshEvent ev = {};
                ev.channel   = (uint8_t)atoi(buf + 1);
                ev.magnitude = 90;
                ev.originId  = pulleys::identity_id();
                components_on_mesh_event(ev);
            } else if (buf[0] == 'g' && n > 2 && buf[1] == ' ') {
                // "g goal_component_activated" -> inject a game event by name:
                // a component event, a device's input, a counter's _reached
                gamestate_input(buf + 2);
            } else if (strcmp(buf, "PARAM") == 0) {
                for (uint8_t i = 0; i < gamestate_param_count(); i++)
                    Serial.printf("  [GAME] %s = %.4g\n", gamestate_param_name(i), gamestate_param_value(i));
            } else if (strncmp(buf, "PARAM ", 6) == 0) {
                // "PARAM round_time 90" -> set a param live (durations in seconds)
                char name[24] = {};
                float value = 0;
                if (sscanf(buf + 6, "%23s %f", name, &value) != 2 || !gamestate_set_param(name, value))
                    Serial.println("  [GAME] usage: PARAM <name> <value>  (a param with a min and max)");
            } else if (buf[0] == 'Q') {
                // The queue sign, or the monitor behind it, without the secret taps
                signui_show(!signui_showing());
            } else if (buf[0] == 'V') {
                devices_print();
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
        signui_note_touch();
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

#if defined(ARBITER_BOARD_CROWPANEL7)
    // ── Panel microcontroller ─────────────────────────────────────────────────
    // The STC8 holds the touch controller unpowered until it is told otherwise;
    // Elecrow's bring-up also pulses GPIO1 low to latch the GT911 address.
    {
        auto mcu = [](uint8_t cmd) {
            Wire.beginTransmission(PANEL_MCU_ADDR);
            Wire.write(cmd);
            return Wire.endTransmission() == 0;
        };
        auto present = [](uint8_t a) {
            Wire.beginTransmission(a);
            return Wire.endTransmission() == 0;
        };
        for (int tries = 0; tries < 20 && !(present(PANEL_MCU_ADDR) && present(pulleys::GT911_ADDR)); tries++) {
            mcu(PANEL_MCU_TOUCH_ON);
            pinMode(1, OUTPUT);
            digitalWrite(1, LOW);
            delay(120);
            pinMode(1, INPUT);
            delay(100);
        }
        bool bl = mcu(ARBITER_BACKLIGHT);
        Serial.printf("  [PANEL] MCU 0x%02X %s, backlight=%d %s\n", PANEL_MCU_ADDR,
                      present(PANEL_MCU_ADDR) ? "present" : "MISSING",
                      ARBITER_BACKLIGHT, bl ? "set" : "FAILED");
    }
    // BM8563 (PCF8563-compatible) at the same 0x51 address.
    s_rtc.setSecondsReg(pulleys::PCF8563_SECONDS);
    s_rtc.begin();
    clocksrc_init(&s_rtc);
#else
    // ── Real-time clock ───────────────────────────────────────────────────────
    // Confirmed a PCF85063 by watching register 0x04 tick in BCD. Seconds at
    // 0x04 rather than 0x02 is what tells it apart from a PCF8563.
    s_rtc.begin();
    clocksrc_init(&s_rtc);
#endif

    s_touch.setBounds(LCD_WIDTH, LCD_HEIGHT);
    if (!s_touch.begin()) Serial.println("  [TOUCH] GT911 not found — clock UI unusable");

#if !defined(ARBITER_BOARD_CROWPANEL7)
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
#endif

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
    pulleys::whoami_reply();
    pulleys::mesh_init(pulleys::MESH_ORIGIN_ARBITER, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::census_set_detail("monitor");
#ifdef MESH_OBSERVE_ONLY
    pulleys::mesh_set_relay(false);
    Serial.println("  [MESH] observe-only: relaying disabled");
#endif
    monitor_init();
    installcfg_init();
    installation_init();
#if !defined(ARBITER_BOARD_CROWPANEL7)   // no SD card on this board
    eventlog_init(&s_rtc);
#endif
    // The monitor has one hook slot; the SD log, the mapping screen and the
    // game all want every event. The log keeps every pull, mapping or not; the
    // game gets none while the mapping screen holds the ropes. Remap acks are
    // not pulls, and only the mapping screen cares about them.
    monitor_on_event([](const pulleys::MeshEvent& ev, bool relayed) {
        bool pull = pulleys::mesh_event_is_pull(ev);
        if (pull) eventlog_record(ev, relayed);
        if (installui_on_event(ev)) return;
        if (pull) components_on_mesh_event(ev);
    });
    ui_init();
    clockui_init(&s_rtc);
    installui_init();
    signui_init(&display, &s_touch);   // the sign is up from boot, if the game has one
    // Last, so the game's opening cues go out onto a mesh that is up.
    devices_init();
    components_init();
    gameconfig_init();
    Serial.printf("Arbiter (mesh monitor) ready — %s\n", pulleys::identity_name());
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t last_tick = 0;
    static uint32_t last_ui   = 0;
    uint32_t now = millis();

    installmap_tick();
    pulleys::mesh_poll();
    pulleys::census_poll();
    gameconfig_poll();
    gamestate_tick();
    devices_tick();
    components_tick();
    installcfg_tick();
    installation_tick();
    handleSerial();

    // A targeted update owns the board while it runs. The panel holds its last frame rather
    // than being redrawn: LVGL has nothing useful to say about a flash write,
    // and the radio would rather have the time.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        return;
    }

    eventlog_tick();
    clocksrc_tick();

    if (now - last_tick >= 1000) {
        last_tick = now;
        monitor_tick();
    }

    clockui_tick();
    installui_tick();
    bool modal = clockui_is_open() || installui_is_open();
    if (!modal) ui_tick();
    // The queue sign draws itself; LVGL must not paint over it meanwhile.
    if (!signui_tick(now, modal)) lv_timer_handler();

    if (now - last_ui >= 750 && !modal) {
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
                if (n.known) {
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
