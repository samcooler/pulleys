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

#define MAX_BRIGHTNESS     15
#define LED_FPS            60
#define MATE_COOLDOWN_MS   30000
#define NUM_SLOTS          4
#define ROWS_PER_SLOT      8       // 32 rows / 4 slots
#define COLS               8

// ── Globals ───────────────────────────────────────────────────────────────────
// Which pattern type to use for all station slots
static pulleys::PatternType stationPatternType = pulleys::PATTERN_PILLOW_SEESAW;

static CRGB leds[LED_COUNT];
static pulleys::ProximityTracker proximity;

// 4 culture slots rendered via PatternSlot
static pulleys::PatternSlot patSlots[NUM_SLOTS];
static PulleysCulture slotsOld[NUM_SLOTS];  // previous culture for fade-out
static uint32_t slotTransStart[NUM_SLOTS];  // millis when transition began (0 = none)

// Pending slot update from BLE callback (avoids race with render loop)
static volatile int8_t   pendingSlot = -1;       // -1 = no pending update
static PulleysCulture    pendingCulture;
static PulleysCulture    pendingOld;

static uint16_t cooldownIds[32];
static uint32_t cooldownTimes[32];
static uint8_t  cooldownCount = 0;

// ── Culture exchange callback ─────────────────────────────────────────────────
static void onZoneChange(const pulleys::TrackedDevice& dev,
                         pulleys::ProximityZone oldZone,
                         pulleys::ProximityZone newZone) {
    if (dev.deviceType != PULLEYS_TYPE_TRAVELER) return;

    if (newZone == pulleys::ZONE_CLOSE) {
        uint32_t now = millis();
        for (uint8_t i = 0; i < cooldownCount; i++) {
            if (cooldownIds[i] == dev.deviceId && (now - cooldownTimes[i]) < MATE_COOLDOWN_MS) {
                Serial.printf("\n☆ T-%04X CLOSE — cooldown (%lus left)\n",
                              dev.deviceId, (MATE_COOLDOWN_MS - (now - cooldownTimes[i])) / 1000);
                return;
            }
        }

        // Queue slot update for main loop (avoid race with render)
        uint8_t s = random(NUM_SLOTS);
        pendingOld = patSlots[s].culture;
        pendingCulture = dev.culture;
        pendingSlot = s;  // atomic write signals main loop

        Serial.printf("\n★ T-%04X → slot %d: %s/%s %.2fHz\n",
                      dev.deviceId, s,
                      pulleys::color_name(dev.culture.colorA),
                      pulleys::color_name(dev.culture.colorB),
                      pulleys::culture_osc_to_hz(dev.culture.oscillation));

        // Record cooldown (prune expired entries first)
        uint32_t now2 = now;
        uint8_t writeIdx = 0;
        for (uint8_t i = 0; i < cooldownCount; i++) {
            if ((now2 - cooldownTimes[i]) < MATE_COOLDOWN_MS * 2) {
                cooldownIds[writeIdx] = cooldownIds[i];
                cooldownTimes[writeIdx] = cooldownTimes[i];
                writeIdx++;
            }
        }
        cooldownCount = writeIdx;

        bool found = false;
        for (uint8_t i = 0; i < cooldownCount; i++) {
            if (cooldownIds[i] == dev.deviceId) { cooldownTimes[i] = now2; found = true; break; }
        }
        if (!found && cooldownCount < 32) {
            cooldownIds[cooldownCount] = dev.deviceId;
            cooldownTimes[cooldownCount] = now2;
            cooldownCount++;
        }
    }
    if (newZone == pulleys::ZONE_GONE && oldZone >= pulleys::ZONE_NEAR) {
        Serial.printf("  T-%04X departed.\n", dev.deviceId);
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

    // 4 random culture slots using PatternSlot
    randomSeed(esp_random());
    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
        patSlots[i].buffer = leds + i * ROWS_PER_SLOT * COLS;
        patSlots[i].serpentine = true;
        patSlots[i].maxBri = MAX_BRIGHTNESS;
        patSlots[i].init(stationPatternType, ROWS_PER_SLOT, COLS);
        patSlots[i].culture = pulleys::culture_random();
        slotTransStart[i] = 0;
        char label[8];
        snprintf(label, sizeof(label), "slot%d", i);
        pulleys::culture_print(label, patSlots[i].culture);
    }

    // LEDs — 8 cols x 32 rows, serpentine
    FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, LED_COUNT);
    FastLED.setBrightness(255);

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
    Serial.println("Station ready.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastLed   = 0;
    static uint32_t lastPrune = 0;
    static uint32_t lastReport = 0;
    uint32_t now = millis();

    // Apply pending slot update from BLE callback (thread-safe handoff)
    if (pendingSlot >= 0) {
        int8_t s = pendingSlot;
        pendingSlot = -1;  // clear first to avoid re-entry
        slotsOld[s] = pendingOld;
        patSlots[s].culture = pendingCulture;
        slotTransStart[s] = now;
    }

    // LED pattern update (~60 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        float t = now / 1000.0f;
        float dt = 1.0f / LED_FPS;

        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            // ── Transition state ──
            // Phase: 0-1s fade old to black, 1-2s fade new in at 3x, 2-6s settle to 1x
            float briMul = 1.0f;
            bool useOld = false;
            if (slotTransStart[s] != 0) {
                float elapsed = (now - slotTransStart[s]) / 1000.0f;
                if (elapsed < 1.0f) {
                    briMul = 1.0f - elapsed;
                    useOld = true;
                } else if (elapsed < 2.0f) {
                    briMul = (elapsed - 1.0f) * 3.0f;
                } else if (elapsed < 6.0f) {
                    briMul = 3.0f - (elapsed - 2.0f) * 0.5f;
                } else {
                    slotTransStart[s] = 0;
                }
            }

            // During fade-out, temporarily swap in old culture
            PulleysCulture saved;
            if (useOld) {
                saved = patSlots[s].culture;
                patSlots[s].culture = slotsOld[s];
            }

            // Render pattern into this slot's region
            pulleys::pattern_slot_update(patSlots[s], dt, t);

            // Restore culture after fade-out render
            if (useOld) {
                patSlots[s].culture = saved;
            }

            // Apply transition brightness as post-multiply
            if (briMul != 1.0f) {
                uint16_t numPx = ROWS_PER_SLOT * COLS;
                CRGB* buf = patSlots[s].buffer;
                if (briMul > 1.0f) {
                    // Brighten: scale up each channel (capped at 255)
                    for (uint16_t i = 0; i < numPx; i++) {
                        buf[i].r = (uint8_t)min(255.0f, buf[i].r * briMul);
                        buf[i].g = (uint8_t)min(255.0f, buf[i].g * briMul);
                        buf[i].b = (uint8_t)min(255.0f, buf[i].b * briMul);
                    }
                } else {
                    uint8_t sc = (uint8_t)(briMul * 255.0f);
                    for (uint16_t i = 0; i < numPx; i++) {
                        buf[i].nscale8(sc);
                    }
                }
            }
        }
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
