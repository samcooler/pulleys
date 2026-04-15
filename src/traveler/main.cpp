#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
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

#define MAX_BRIGHTNESS     21
#define BEACON_INTERVAL_MS 500
#define LED_FPS            30
#define IMU_INTERVAL_MS    100

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
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
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

    // WiFi + OTA disabled for now
    // WiFi.mode(WIFI_STA);
    // WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // ArduinoOTA.begin();

    Serial.println("Traveler ready.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastBeacon = 0;
    static uint32_t lastLed    = 0;
    static uint32_t lastPrune  = 0;
    static uint32_t lastImu    = 0;
    uint32_t now = millis();

    // LED pattern update (~30 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        pattern.update();
        FastLED.show();
    }

    // BLE beacon update (update payload without stop/start cycle)
    if (now - lastBeacon >= BEACON_INTERVAL_MS) {
        lastBeacon = now;
        counter++;
        updatePayload();
        Serial.printf("Beacon #%lu\n", counter);
    }

    // Prune stale proximity entries (~1Hz)
    if (now - lastPrune >= 1000) {
        lastPrune = now;
        proximity.pruneStale();
    }

    // IMU reading
    if (now - lastImu >= IMU_INTERVAL_MS) {
        lastImu = now;
        pulleys::AccelData accel;
        if (imu.read(accel)) {
            // Feed gravity to pattern renderer — center moves toward tilt
            pattern.setGravity(accel.x, accel.y);

            // Feed to ritual detector for gesture recognition
            ritual.update(accel.x, accel.y, accel.z, 0, 0, 0);

            // Serial output
            Serial.printf("  [IMU] ax=%.2f ay=%.2f az=%.2f\n", accel.x, accel.y, accel.z);
        }
    }

    // OTA update check (disabled)
    // ArduinoOTA.handle();
}
