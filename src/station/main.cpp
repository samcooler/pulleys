#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
#include <pulleys_protocol.h>
#include <pulleys_identity.h>
#include <pulleys_culture.h>
#include <pulleys_proximity.h>

// ── Config (LED_PIN, LED_COUNT set via build_flags in platformio.ini) ─────────
#ifndef LED_PIN
  #define LED_PIN   48
#endif
#ifndef LED_COUNT
  #define LED_COUNT 10
#endif

#define MAX_BRIGHTNESS     15
#define LED_FPS            30
#define MATE_COOLDOWN_MS   30000
#define NUM_SLOTS          4
#define ROWS_PER_SLOT      8       // 32 rows / 4 slots
#define COLS               8

// ── Globals ───────────────────────────────────────────────────────────────────
static CRGB leds[LED_COUNT];
static pulleys::ProximityTracker proximity;

// 4 culture slots — each has its own two colors and frequency
static PulleysCulture slots[NUM_SLOTS];
static uint8_t nextSlot = 0;  // round-robin replacement

static uint16_t cooldownIds[32];
static uint32_t cooldownTimes[32];
static uint8_t  cooldownCount = 0;

// ── XY mapping for serpentine 8-wide matrix ───────────────────────────────────
static inline uint16_t xyToIndex(uint8_t row, uint8_t col) {
    if (row & 1) col = (COLS - 1) - col;  // odd rows reversed
    return (uint16_t)row * COLS + col;
}

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

        // Overwrite the next slot with the traveler's culture (no blending)
        uint8_t s = nextSlot;
        slots[s] = dev.culture;
        nextSlot = (nextSlot + 1) % NUM_SLOTS;

        Serial.printf("\n★ T-%04X → slot %d: %s/%s %.2fHz\n",
                      dev.deviceId, s,
                      pulleys::color_name(dev.culture.colorA),
                      pulleys::color_name(dev.culture.colorB),
                      pulleys::culture_osc_to_hz(dev.culture.oscillation));

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

    // 4 random culture slots
    randomSeed(esp_random());
    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
        slots[i] = pulleys::culture_random();
        char label[8];
        snprintf(label, sizeof(label), "slot%d", i);
        pulleys::culture_print(label, slots[i]);
    }

    // LEDs — 8 cols x 32 rows, serpentine
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
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

    // LED pattern update (~30 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        float t = now / 1000.0f;

        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            float hz = pulleys::culture_osc_to_hz(slots[s].oscillation);
            // See-saw: left half and right half oscillate in antiphase
            float wave = (sinf(t * hz * 2.0f * (float)M_PI) + 1.0f) * 0.5f;  // 0-1
            float waveInv = 1.0f - wave;

            CRGB cA(slots[s].colorA.r, slots[s].colorA.g, slots[s].colorA.b);
            CRGB cB(slots[s].colorB.r, slots[s].colorB.g, slots[s].colorB.b);

            // Left half: blend from A toward B
            CRGB leftC;
            leftC.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * wave);
            leftC.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * wave);
            leftC.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * wave);

            // Right half: blend from B toward A (antiphase)
            CRGB rightC;
            rightC.r = (uint8_t)(cA.r + (float)(cB.r - cA.r) * waveInv);
            rightC.g = (uint8_t)(cA.g + (float)(cB.g - cA.g) * waveInv);
            rightC.b = (uint8_t)(cA.b + (float)(cB.b - cA.b) * waveInv);

            uint8_t startRow = s * ROWS_PER_SLOT;
            // Single pillow per slot, split left/right by sharp center line
            float cxMid = (COLS - 1) * 0.5f;            // horizontal center
            float cySlot = (ROWS_PER_SLOT - 1) * 0.5f;  // vertical center of slot

            for (uint8_t r = 0; r < ROWS_PER_SLOT; r++) {
                uint8_t row = startRow + r;
                // Vertical pillow: raised cosine across slot height
                float vy = cosf((r - cySlot) / cySlot * (float)M_PI * 0.5f);
                vy = vy * vy;

                for (uint8_t c = 0; c < COLS; c++) {
                    uint16_t idx = xyToIndex(row, c);
                    // Horizontal pillow across full width
                    float dx = (c - cxMid) / (cxMid + 0.5f);
                    float vx = cosf(dx * (float)M_PI * 0.5f);
                    vx = vx * vx;

                    uint8_t pillow = (uint8_t)(vy * vx * MAX_BRIGHTNESS);
                    // Sharp split: left = leftC, right = rightC
                    leds[idx] = (c < COLS / 2) ? leftC : rightC;
                    leds[idx].nscale8(pillow);
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
