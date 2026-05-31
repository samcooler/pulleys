#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
// #include <WiFi.h>
// #include <ArduinoOTA.h>
#include <pulleys_protocol.h>
#include <pulleys_identity.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_proximity.h>
#include <pulleys_ritual.h>
#include <pulleys_imu.h>

// ── Config (LED_PIN, LED_COUNT set via build_flags in platformio.ini) ─────────
#ifndef LED_PIN
  #define LED_PIN   14
#endif
#ifndef LED_COUNT
  #define LED_COUNT 64
#endif

#define MAX_BRIGHTNESS     32
#define BEACON_INTERVAL_MS 500
#define LED_FPS            30
#define IMU_INTERVAL_MS    100
#define SLEEP_TIMEOUT_MS   240000
#define MOTION_THRESHOLD   0.05f   // g-force delta to count as motion
#define FADE_DURATION_MS   1000    // fade in/out duration
#define DREAM_INTERVAL_MS  30000   // base time between dreams
#define DREAM_JITTER_MS    5000    // ±randomness added to dream interval and sleep timeout
#define DREAM_LIT_MS       2000    // how long the dream stays lit
#define IMU_INT1_PIN       GPIO_NUM_10  // QMI8658 INT1 → GPIO10
#define WOM_THRESHOLD      30          // Wake-on-Motion threshold (0-255, lower=more sensitive)

// Battery voltage via resistor divider on GPIO2 (ADC1_CH1)
// Divider: VBAT → R1 → GPIO2 → R2 → GND  (matched pair, so ratio = 2.0)
#define VBAT_PIN           1
#define VBAT_DIVIDER_RATIO 2.0f     // (R1+R2)/R2 — update if resistors differ
#define VBAT_ADC_REF       3.3f     // ESP32-S3 ADC reference voltage

static const bool DEBUG_FLASH = false;  // set true to enable LED debug flashes

// WiFi credentials for OTA updates (disabled for now)
// #define WIFI_SSID     "Blossom 2.4 GHz"
// #define WIFI_PASSWORD "pollinate"

// BLE advertising interval in 0.625ms units
#define BLE_INTERVAL_UNITS (BEACON_INTERVAL_MS * 1000 / 625)

// ── Globals ───────────────────────────────────────────────────────────────────
static CRGB leds[LED_COUNT];
static pulleys::PatternRenderer pattern;
static pulleys::ProximityTracker proximity;
static pulleys::RitualDetector ritual;
static pulleys::IMU imu;

static PulleysCulture myCulture;
static uint32_t counter = 0;
static NimBLEAdvertising* pAdv = nullptr;
static pulleys::PatternType travelerPatternType = pulleys::PATTERN_SHAPE;

// ── Sleep / wake state machine ────────────────────────────────────────────────
enum TravelerState { AWAKE, FADING_OUT, ASLEEP, FADING_IN, DREAM_IN, DREAM_LIT, DREAM_OUT };
static TravelerState travelerState = AWAKE;
static uint32_t lastMotionMs = 0;
static uint32_t fadeStartMs  = 0;
static uint32_t sleepStartMs = 0;  // when we entered ASLEEP
static bool imuBaselineValid = false;  // reset after sleep to avoid false motion
static uint32_t imuResumeMs = 0;      // when IMU was restored after sleep

// ── Forward declarations ──────────────────────────────────────────────────────
static void updatePayload();

// ── BLE stop/start helpers ────────────────────────────────────────────────────
static void bleStopAll() {
    pAdv->stop();
    NimBLEDevice::getScan()->stop();
    Serial.println("  [BLE] Stopped adv + scan");
}

static void bleStartAll() {
    updatePayload();
    pAdv->start();
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->start(0, false, false);
    Serial.println("  [BLE] Resumed adv + scan");
}

// ── Light sleep loop — runs during ASLEEP state ──────────────────────────────
// Uses QMI8658 Wake-on-Motion interrupt on INT1 (GPIO10) for instant motion wake,
// plus a timer for dream triggering.

// LED debug flash helper — shows a color briefly for visual debugging
static void debugFlash(CRGB color, int count = 1) {
    static const uint8_t CENTER[4] = { 27, 28, 35, 36 };  // center 4 of 8×8
    CRGB dimColor = color;
    dimColor.nscale8(3);
    for (int i = 0; i < count; i++) {
        for (uint8_t idx : CENTER) leds[idx] = dimColor;
        FastLED.show();
        delay(80);
        for (uint8_t idx : CENTER) leds[idx] = CRGB::Black;
        FastLED.show();
        if (i < count - 1) delay(60);
    }
}

static void lightSleepLoop() {
    Serial.println("  [SLEEP] Configuring WoM + entering light sleep");

    // Configure QMI8658 for Wake-on-Motion
    bool womOk = imu.configWakeOnMotion(WOM_THRESHOLD);
    Serial.printf("  [SLEEP] WoM config: %s\n", womOk ? "OK" : "FAILED");

    // Configure GPIO10 (INT1) and GPIO13 (INT2) as inputs with pullup
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << IMU_INT1_PIN) | (1ULL << GPIO_NUM_13);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // Check both INT pins after WoM config
    Serial.printf("  [SLEEP] After WoM config: INT1(GPIO10)=%d INT2(GPIO13)=%d STATUS1=0x%02X\n",
                  gpio_get_level(IMU_INT1_PIN), gpio_get_level(GPIO_NUM_13), imu.readReg(0x2F));

    for (int i = 0; i < 50; i++) {
        imu.checkWomEvent();
        delay(20);
        if (gpio_get_level(IMU_INT1_PIN) == 1 && gpio_get_level(GPIO_NUM_13) == 1) break;
    }
    Serial.printf("  [SLEEP] INT1: %d  INT2: %d\n",
                  gpio_get_level(IMU_INT1_PIN), gpio_get_level(GPIO_NUM_13));

    // Enable wake on BOTH INT pins (we'll figure out which one WoM uses)
    gpio_wakeup_enable(IMU_INT1_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(GPIO_NUM_13, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    uint32_t dreamMs = DREAM_INTERVAL_MS + (esp_random() % (2 * DREAM_JITTER_MS + 1)) - DREAM_JITTER_MS;
    esp_sleep_enable_timer_wakeup((uint64_t)dreamMs * 1000ULL);

    Serial.println("  [SLEEP] Entering light sleep loop");
    Serial.flush();

    if (DEBUG_FLASH) debugFlash(CRGB::Red);  // entering sleep loop

    while (travelerState == ASLEEP) {
        // Ensure both INT pins are HIGH before sleeping
        for (int i = 0; i < 50; i++) {
            imu.checkWomEvent();
            delay(20);
            if (gpio_get_level(IMU_INT1_PIN) == 1 && gpio_get_level(GPIO_NUM_13) == 1) break;
        }

        esp_light_sleep_start();

        uint32_t now = millis();
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

        if (cause == ESP_SLEEP_WAKEUP_GPIO) {
            if (DEBUG_FLASH) debugFlash(CRGB::Green);  // GPIO wake (motion)
            lastMotionMs = now;
            travelerState = FADING_IN;
            fadeStartMs = now;
            bleStartAll();
            break;
        } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            if (DEBUG_FLASH) debugFlash(CRGB::Blue);  // timer wake (dream)
            travelerState = DREAM_IN;
            fadeStartMs = now;
            break;
        } else {
            if (DEBUG_FLASH) debugFlash(CRGB::Yellow);  // GPIO already LOW — not sleeping
            imu.checkWomEvent();
            delay(50);
        }
    }

    if (DEBUG_FLASH) debugFlash(CRGB::White);  // exiting sleep loop

    // Clean up wake sources
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    gpio_wakeup_disable(IMU_INT1_PIN);
    gpio_wakeup_disable(GPIO_NUM_13);

    bool isDream = (travelerState == DREAM_IN);

    if (!isDream) {
        // Motion wake — restore full IMU + serial
        imu.restoreNormalMode(11, 12);
        imuBaselineValid = false;
        imuResumeMs = millis();
    }

    // Try to recover USB serial after light sleep
    Serial.end();
    delay(50);
    Serial.begin(115200);
    delay(200);
}

// ── BLE advertising payload ───────────────────────────────────────────────────
static void updatePayload() {
    PulleysPacket pkt;
    pkt.deviceType = PULLEYS_TYPE_TRAVELER;
    pkt.deviceId   = pulleys::identity_id();
    pkt.culture    = myCulture;
    pkt.counter    = counter;

    uint8_t mfr[PULLEYS_MFR_LEN];
    pulleys_serialize(&pkt, mfr);

    NimBLEAdvertisementData data;
    data.setManufacturerData(std::string((char*)mfr, sizeof(mfr)));
    pAdv->setAdvertisementData(data);
}

// ── BLE scan callback — detect nearby Stations ───────────────────────────────
class StationScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string raw = dev->getManufacturerData();
        PulleysPacket pkt;
        if (!pulleys_parse((const uint8_t*)raw.data(), raw.size(), &pkt)) return;
        if (pkt.deviceType != PULLEYS_TYPE_STATION) return;

        proximity.update(pkt, dev->getRSSI());

        // TODO: When a station is CLOSE, absorb some of its culture
        // myCulture = pulleys::culture_blend(myCulture, pkt.culture, 0.05f);
    }
};

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // Identity — BLE must init first
    NimBLEDevice::init("");
    NimBLEDevice::setPower(-6);  // reduce TX power: saves battery, tightens proximity zones
    pulleys::identity_init(PULLEYS_TYPE_TRAVELER);
    pulleys::identity_print_banner(PULLEYS_TYPE_TRAVELER);

    // Culture — start with a random one
    randomSeed(esp_random());
    myCulture = pulleys::culture_random();
    Serial.println("Starting culture:");
    pulleys::culture_print("mine", myCulture);

    // LEDs
    FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, LED_COUNT);
    FastLED.setBrightness(255);  // brightness controlled by PatternRenderer only


    // Boot preview: show culture colors on centered rows for 1 second
    {
        uint8_t cols = 8;  // 8x8 matrix
        uint8_t rowA = (LED_COUNT / cols) / 2 - 1;  // row 3
        uint8_t rowB = rowA + 1;                     // row 4
        CRGB cA(myCulture.colorA.r, myCulture.colorA.g, myCulture.colorA.b);
        CRGB cB(myCulture.colorB.r, myCulture.colorB.g, myCulture.colorB.b);
        cA.nscale8(MAX_BRIGHTNESS);
        cB.nscale8(MAX_BRIGHTNESS);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        fill_solid(leds + rowA * cols, cols, cA);
        fill_solid(leds + rowB * cols, cols, cB);
        FastLED.show();
        delay(1000);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        FastLED.show();
    }

    pattern.init(leds, LED_COUNT, MAX_BRIGHTNESS);
    pattern.setMatrixSize(8, 8, false); // 8x8, no serpentine
    pattern.setPatternType(travelerPatternType);
    pattern.setDensity(0.2f);  // sparse sparkle — ~20% of pixels lit
    pattern.setCulture(myCulture);

    // BLE advertising
    pAdv = NimBLEDevice::getAdvertising();
    pAdv->setMinInterval(BLE_INTERVAL_UNITS);
    pAdv->setMaxInterval(BLE_INTERVAL_UNITS);
    updatePayload();
    pAdv->start();
    Serial.println("BLE beacon started");

    // BLE scanning (passive, between ad bursts)
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new StationScanCallbacks(), true);
    pScan->setActiveScan(false);
    pScan->setInterval(160);   // 100ms
    pScan->setWindow(80);      // 50ms — leaves room for advertising
    pScan->start(0, false, false);
    Serial.println("Scanning for Stations...");

    // IMU / Ritual
    if (imu.init(11, 12)) {
        Serial.println("  [IMU] Accelerometer ready");
    } else {
        Serial.println("  [IMU] Accelerometer FAILED — patterns will wander randomly");
    }
    ritual.init();

    // ── INT pin diagnostic ──
    // Read GPIO10/13 BEFORE any WoM config to check baseline
    pinMode(10, INPUT_PULLUP);
    pinMode(13, INPUT_PULLUP);
    delay(10);
    Serial.printf("  [DIAG] GPIO10=%d GPIO13=%d (with pullup, before WoM)\n",
                  digitalRead(10), digitalRead(13));
    // Read key IMU registers
    Serial.printf("  [DIAG] CTRL1=0x%02X CTRL7=0x%02X STATUS1=0x%02X\n",
                  imu.readReg(0x02), imu.readReg(0x08), imu.readReg(0x2F));

    // WiFi + OTA disabled for now
    // WiFi.mode(WIFI_STA);
    // WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // ArduinoOTA.begin();

    lastMotionMs = millis();  // don't sleep immediately on boot
    Serial.println("Traveler ready.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static const char* stateNames[] = { "AWAKE", "FADE_OUT", "ASLEEP", "FADE_IN", "DREAM_IN", "DREAM_LIT", "DREAM_OUT" };

void loop() {
    static uint32_t lastBeacon       = 0;
    static uint32_t lastLed          = 0;
    static uint32_t lastPrune        = 0;
    static uint32_t lastImu          = 0;
    static uint32_t lastLog          = 0;
    static uint32_t lastPatternCycle = 0;
    static uint8_t  currentShape     = 0;
    static float    lastDelta        = 0;
    uint32_t now = millis();

    // Cycle through all shapes every 10 seconds
    if (travelerState == AWAKE && now - lastPatternCycle >= 10000) {
        lastPatternCycle = now;
        currentShape = (currentShape + 1) % pulleys::SHAPE_COUNT;
        myCulture.shape = currentShape;
        pattern.setCulture(myCulture);
        Serial.printf("[PATTERN] shape=%u (%s)\n", currentShape, pulleys::shape_name(currentShape));
    }

    // LED pattern update (~30 fps) — skip only when fully asleep
    if (travelerState != ASLEEP && now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        pattern.update();

        // Apply fade multiplier during transitions
        if (travelerState == FADING_OUT || travelerState == FADING_IN ||
            travelerState == DREAM_IN   || travelerState == DREAM_OUT) {
            float elapsed = (float)(now - fadeStartMs);
            float frac = elapsed / (float)FADE_DURATION_MS;
            if (frac > 1.0f) frac = 1.0f;
            bool fadingUp = (travelerState == FADING_IN || travelerState == DREAM_IN);
            uint8_t scale = fadingUp
                ? (uint8_t)(frac * 255.0f)
                : (uint8_t)((1.0f - frac) * 255.0f);
            for (uint16_t i = 0; i < LED_COUNT; i++) {
                leds[i].nscale8(scale);
            }

            // Transition complete?
            if (frac >= 1.0f) {
                if (travelerState == FADING_OUT) {
                    travelerState = ASLEEP;
                    sleepStartMs = now;
                    bleStopAll();
                    fill_solid(leds, LED_COUNT, CRGB::Black);
                    FastLED.show();

                    // Battery indicator: 0 flashes (>80%) to 4 flashes (<20%)
                    float vbat = analogRead(VBAT_PIN) * VBAT_ADC_REF / 4095.0f * VBAT_DIVIDER_RATIO;
                    int pct = constrain((int)((vbat - 3.0f) / 1.2f * 100.0f), 0, 100);
                    CRGB flashColor = (pct >= 80) ? CRGB::Green : CRGB::Red;
                    int flashes = (pct >= 60) ? 1 : (pct >= 40) ? 2 : (pct >= 20) ? 3 : 4;
                    Serial.printf("[SLEEP] vbat=%.2fV (%d%%) — %d flash(es)\n", vbat, pct, flashes);
                    delay(100);
                    debugFlash(flashColor, flashes);

                    Serial.println("[SLEEP] Fade complete — entering light sleep");
                    lightSleepLoop();
                    return;  // re-enter loop() with new state
                } else if (travelerState == FADING_IN) {
                    travelerState = AWAKE;
                    Serial.println("[WAKE] Fade complete — awake");
                } else if (travelerState == DREAM_IN) {
                    travelerState = DREAM_LIT;
                    fadeStartMs = now;  // reuse as lit-start time
                    Serial.println("[DREAM] Lit");
                } else if (travelerState == DREAM_OUT) {
                    travelerState = ASLEEP;
                    sleepStartMs = now;
                    fill_solid(leds, LED_COUNT, CRGB::Black);
                    FastLED.show();
                    Serial.println("[DREAM] Fade out complete — back to light sleep");
                    lightSleepLoop();
                    return;  // re-enter loop() with new state
                }
            }
        }

        // Dream lit phase: hold for DREAM_LIT_MS then fade out
        if (travelerState == DREAM_LIT && (now - fadeStartMs) >= DREAM_LIT_MS) {
            travelerState = DREAM_OUT;
            fadeStartMs = now;
            Serial.println("[DREAM] Fading out");
        }

        FastLED.show();
    }

    // BLE beacon update — only when awake or fading in
    if ((travelerState == AWAKE || travelerState == FADING_IN) && now - lastBeacon >= BEACON_INTERVAL_MS) {
        lastBeacon = now;
        counter++;
        updatePayload();
    }

    // Prune stale proximity entries (~1Hz)
    if (now - lastPrune >= 1000) {
        lastPrune = now;
        proximity.pruneStale();
    }

    // IMU reading + motion detection for sleep/wake
    if (now - lastImu >= IMU_INTERVAL_MS) {
        lastImu = now;
        pulleys::AccelData accel;
        if (imu.read(accel)) {
            static float lastAx = 0, lastAy = 0, lastAz = 0;

            // After returning from sleep, skip motion detection for 500ms
            // (IMU samples are noisy right after restore)
            if (!imuBaselineValid) {
                lastAx = accel.x; lastAy = accel.y; lastAz = accel.z;
                if ((now - imuResumeMs) >= 500) {
                    imuBaselineValid = true;
                }
            } else {

            float dx = accel.x - lastAx;
            float dy = accel.y - lastAy;
            float dz = accel.z - lastAz;
            lastAx = accel.x; lastAy = accel.y; lastAz = accel.z;
            float deltaMag = sqrtf(dx*dx + dy*dy + dz*dz);
            lastDelta = deltaMag;

            if (deltaMag >= MOTION_THRESHOLD) {
                lastMotionMs = now;
                if (travelerState == FADING_OUT) {
                    // Reverse the fade — pick up from current brightness
                    float elapsed = (float)(now - fadeStartMs);
                    float frac = elapsed / (float)FADE_DURATION_MS;
                    if (frac > 1.0f) frac = 1.0f;
                    travelerState = FADING_IN;
                    fadeStartMs = now - (uint32_t)((1.0f - frac) * FADE_DURATION_MS);
                    Serial.println("[WAKE] Motion during fade-out — reversing");
                }
            }

            if (travelerState == AWAKE || travelerState == FADING_IN) {
                // Feed to ritual detector for gesture recognition
                ritual.update(accel.x, accel.y, accel.z, 0, 0, 0);
            }

            } // end else (imuBaselineValid)
        }
    }

    // 1Hz status log
    if (travelerState != ASLEEP && now - lastLog >= 1000) {
        lastLog = now;
        uint32_t sinceMotion = now - lastMotionMs;
        // Battery voltage: raw ADC → scale by divider ratio
        float vbatRaw = analogRead(VBAT_PIN) * VBAT_ADC_REF / 4095.0f;
        float vbat = vbatRaw * VBAT_DIVIDER_RATIO;
        Serial.printf("[%s] delta=%.3f motionAgo=%lums baseline=%s vbat=%.2fV (raw=%.3fV)\n",
                      stateNames[travelerState], lastDelta, sinceMotion,
                      imuBaselineValid ? "ok" : "settling", vbat, vbatRaw);
    }

    // Sleep transition: no motion for SLEEP_TIMEOUT_MS ± jitter
    static uint32_t currentSleepTimeout = SLEEP_TIMEOUT_MS;
    if (travelerState == AWAKE && (now - lastMotionMs) >= currentSleepTimeout) {
        travelerState = FADING_OUT;
        fadeStartMs = now;
        imuBaselineValid = false;  // reset for next wake
        // Pick new random timeout for next awake cycle
        currentSleepTimeout = SLEEP_TIMEOUT_MS + (esp_random() % (2 * DREAM_JITTER_MS + 1)) - DREAM_JITTER_MS;
        Serial.println("[SLEEP] No motion — fading out");
    }

    // OTA update check (disabled)
    // ArduinoOTA.handle();
}
