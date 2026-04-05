#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_identity.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_proximity.h>

// ── Config (LED_PIN, LED_COUNT set via build_flags in platformio.ini) ─────────
#ifndef LED_PIN
  #define LED_PIN   48
#endif
#ifndef LED_COUNT
  #define LED_COUNT 10
#endif

#define MAX_BRIGHTNESS     80
#define LED_FPS            30
#define CULTURE_BLEND_RATIO 0.10f   // how much a visiting traveler influences us
#define MATE_COOLDOWN_MS   30000    // min ms between culture exchanges with same traveler

// ── Globals ───────────────────────────────────────────────────────────────────
static CRGB leds[LED_COUNT];
static pulleys::PatternRenderer pattern;
static pulleys::ProximityTracker proximity;
static PulleysCulture myCulture;
static uint16_t cooldownIds[32];
static uint32_t cooldownTimes[32];
static uint8_t  cooldownCount = 0;

// ── Culture exchange callback ─────────────────────────────────────────────────
static void onZoneChange(const pulleys::TrackedDevice& dev,
                         pulleys::ProximityZone oldZone,
                         pulleys::ProximityZone newZone) {
    if (dev.deviceType != PULLEYS_TYPE_TRAVELER) return;

    if (newZone == pulleys::ZONE_CLOSE) {
        // Check per-traveler cooldown
        uint32_t now = millis();
        for (uint8_t i = 0; i < cooldownCount; i++) {
            if (cooldownIds[i] == dev.deviceId && (now - cooldownTimes[i]) < MATE_COOLDOWN_MS) {
                Serial.printf("\n☆ Traveler T-%04X CLOSE — cooldown (%lus left)\n",
                              dev.deviceId, (MATE_COOLDOWN_MS - (now - cooldownTimes[i])) / 1000);
                return;
            }
        }

        Serial.printf("\n★ Traveler T-%04X arrived CLOSE — absorbing culture!\n", dev.deviceId);
        pulleys::culture_print("theirs", dev.culture);
        pulleys::culture_print("mine  ", myCulture);

        myCulture = pulleys::culture_blend(myCulture, dev.culture, CULTURE_BLEND_RATIO);
        pattern.setCulture(myCulture);

        pulleys::culture_print("merged", myCulture);
        Serial.println();

        // Record cooldown
        bool found = false;
        for (uint8_t i = 0; i < cooldownCount; i++) {
            if (cooldownIds[i] == dev.deviceId) { cooldownTimes[i] = now; found = true; break; }
        }
        if (!found && cooldownCount < 32) {
            cooldownIds[cooldownCount] = dev.deviceId;
            cooldownTimes[cooldownCount] = now;
            cooldownCount++;
        }
    }
    if (newZone == pulleys::ZONE_GONE && oldZone >= pulleys::ZONE_NEAR) {
        Serial.printf("  Traveler T-%04X departed.\n", dev.deviceId);
    }
}

// ── BLE scan callback ─────────────────────────────────────────────────────────
class TravelerCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string raw = dev->getManufacturerData();
        PulleysPacket pkt;
        if (!pulleys_parse((const uint8_t*)raw.data(), raw.size(), &pkt)) return;
        if (pkt.deviceType != PULLEYS_TYPE_TRAVELER) return;

        proximity.update(pkt, dev->getRSSI());
    }
};

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // Identity
    NimBLEDevice::init("");
    pulleys::identity_init(PULLEYS_TYPE_STATION);
    pulleys::identity_print_banner(PULLEYS_TYPE_STATION);

    // Culture — start with a random one
    randomSeed(esp_random());
    myCulture = pulleys::culture_random();
    Serial.println("Starting culture:");
    pulleys::culture_print("mine", myCulture);

    // LEDs
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(255);  // brightness controlled by PatternRenderer only
    pattern.init(leds, LED_COUNT, MAX_BRIGHTNESS);
    pattern.setCulture(myCulture);

    // Proximity
    proximity.setZoneChangeCallback(onZoneChange);

    // BLE scanning
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new TravelerCallbacks(), true);
    pScan->setActiveScan(false);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(0, false, false);
    Serial.println("Scanning for Travelers...");

    // TODO: OTA update initialization would go here
    // ArduinoOTA.setHostname(pulleys::identity_name());
    // ArduinoOTA.begin();

    Serial.println("Station ready.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastLed   = 0;
    static uint32_t lastPrune = 0;
    static uint32_t lastReport = 0;
    uint32_t now = millis();

    // LED pattern update (~30 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        pattern.update();
        FastLED.show();
    }

    // Prune stale proximity entries (~1Hz)
    if (now - lastPrune >= 1000) {
        lastPrune = now;
        proximity.pruneStale();
    }

    // Traveler summary every 2 seconds
    if (now - lastReport >= 2000) {
        lastReport = now;
        uint8_t count = 0;
        proximity.forEachActive([&count](const pulleys::TrackedDevice& d) {
            if (d.deviceType == PULLEYS_TYPE_TRAVELER) count++;
        });
        if (count > 0) {
            Serial.printf("── %d traveler(s) heard ──\n", count);
            proximity.forEachActive([](const pulleys::TrackedDevice& d) {
                if (d.deviceType != PULLEYS_TYPE_TRAVELER) return;
                Serial.printf("  T-%04X %-5s %4.0fdBm  ",
                              d.deviceId,
                              pulleys::zone_name(d.zone),
                              d.rssiSmooth);
                Serial.printf("%s/%s %.2fHz\n",
                              pulleys::color_name(d.culture.colorA),
                              pulleys::color_name(d.culture.colorB),
                              pulleys::culture_osc_to_hz(d.culture.oscillation));
            });
        }
    }
}
