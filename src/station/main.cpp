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
#define LED_FPS            60
#define MATE_COOLDOWN_MS   30000
#define NUM_SLOTS          4
#define ROWS_PER_SLOT      8       // 32 rows / 4 slots
#define COLS               8

// ── Globals ───────────────────────────────────────────────────────────────────
static CRGB leds[LED_COUNT];
static pulleys::ProximityTracker proximity;

// 4 culture slots — each has its own two colors and frequency
static PulleysCulture slots[NUM_SLOTS];
static PulleysCulture slotsOld[NUM_SLOTS];  // previous culture for fade-out
static uint32_t slotTransStart[NUM_SLOTS];  // millis when transition began (0 = none)

// Pending slot update from BLE callback (avoids race with render loop)
static volatile int8_t   pendingSlot = -1;       // -1 = no pending update
static PulleysCulture    pendingCulture;
static PulleysCulture    pendingOld;

static uint16_t cooldownIds[32];
static uint32_t cooldownTimes[32];
static uint8_t  cooldownCount = 0;

// ── Precomputed pillow brightness table (static, computed once) ───────────────
static uint8_t pillowMap[ROWS_PER_SLOT][COLS];  // 0-255 pillow factor per pixel

static void initPillowMap() {
    float cxMid = (COLS - 1) * 0.5f;
    float cySlot = (ROWS_PER_SLOT - 1) * 0.5f;
    for (uint8_t r = 0; r < ROWS_PER_SLOT; r++) {
        float vy = cosf((r - cySlot) / cySlot * (float)M_PI * 0.5f);
        vy = vy * vy;
        for (uint8_t c = 0; c < COLS; c++) {
            float dx = (c - cxMid) / (cxMid + 0.5f);
            float vx = cosf(dx * (float)M_PI * 0.5f);
            vx = vx * vx;
            pillowMap[r][c] = (uint8_t)(vy * vx * 255.0f);
        }
    }
}

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

        // Queue slot update for main loop (avoid race with render)
        uint8_t s = random(NUM_SLOTS);
        pendingOld = slots[s];
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

    // 4 random culture slots
    randomSeed(esp_random());
    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
        slots[i] = pulleys::culture_random();
        slotTransStart[i] = 0;
        char label[8];
        snprintf(label, sizeof(label), "slot%d", i);
        pulleys::culture_print(label, slots[i]);
    }

    // LEDs — 8 cols x 32 rows, serpentine
    FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, LED_COUNT);
    FastLED.setBrightness(255);
    initPillowMap();

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
        slots[s] = pendingCulture;
        slotTransStart[s] = now;
    }

    // LED pattern update (~60 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        float t = now / 1000.0f;

        for (uint8_t s = 0; s < NUM_SLOTS; s++) {
            // ── Transition state ──
            // Phase: 0-1s fade old to black, 1-2s fade new in at 3x, 2-6s settle to 1x
            float briMul = 1.0f;  // brightness multiplier
            PulleysCulture* cur = &slots[s];
            bool useOld = false;
            if (slotTransStart[s] != 0) {
                float elapsed = (now - slotTransStart[s]) / 1000.0f;
                if (elapsed < 1.0f) {
                    // Fade out old culture
                    briMul = 1.0f - elapsed;  // 1→0
                    useOld = true;
                } else if (elapsed < 2.0f) {
                    // Fade in new culture at 3x brightness
                    briMul = (elapsed - 1.0f) * 3.0f;  // 0→3
                } else if (elapsed < 6.0f) {
                    // Settle from 3x down to 1x
                    briMul = 3.0f - (elapsed - 2.0f) * 0.5f;  // 3→1
                } else {
                    slotTransStart[s] = 0;  // transition done
                }
            }
            PulleysCulture* active = useOld ? &slotsOld[s] : cur;

            float hz = pulleys::culture_osc_to_hz(active->oscillation);
            // See-saw: left half and right half oscillate in antiphase
            float wave = (sinf(t * hz * 2.0f * (float)M_PI) + 1.0f) * 0.5f;  // 0-1
            float waveInv = 1.0f - wave;

            CRGB cA(active->colorA.r, active->colorA.g, active->colorA.b);
            CRGB cB(active->colorB.r, active->colorB.g, active->colorB.b);

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

            for (uint8_t r = 0; r < ROWS_PER_SLOT; r++) {
                uint8_t row = startRow + r;
                for (uint8_t c = 0; c < COLS; c++) {
                    uint16_t idx = xyToIndex(row, c);
                    uint8_t bScale = (uint8_t)(briMul * MAX_BRIGHTNESS);
                    if (bScale > 3 * MAX_BRIGHTNESS) bScale = 3 * MAX_BRIGHTNESS;
                    uint8_t pillow = scale8(pillowMap[r][c], bScale);
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
