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

#define MAX_BRIGHTNESS     38    // wanderer peak; slot.maxBri set to 255 so shape renders full range
#define LED_FPS            60
#define MATE_COOLDOWN_MS   30000
#define NUM_SLOTS          4
#define ROWS_PER_SLOT      8       // 32 rows / 4 slots
#define COLS               8

// ── Globals ───────────────────────────────────────────────────────────────────
// Which pattern type to use for all station slots
static pulleys::PatternType stationPatternType = pulleys::PATTERN_SHAPE;

static CRGB leds[LED_COUNT];
static pulleys::ProximityTracker proximity;

// 4 culture slots rendered via PatternSlot
static pulleys::PatternSlot patSlots[NUM_SLOTS];
static pulleys::Wanderer    briWanderers[NUM_SLOTS];

// Absorption sequence state machine
enum AbsPhase : uint8_t {
    ABS_IDLE = 0,
    ABS_DIM,      // 0.5s  — all slots fade to black
    ABS_FLASH,    // 3.0s  — all slots show new culture at full brightness
    ABS_RESOLVE,  // 1.0s  — non-chosen slots fade out
    ABS_RESTORE,  // 1.5s  — non-chosen slots fade in with old cultures
};
static AbsPhase       absPhase      = ABS_IDLE;
static uint32_t       absPhaseStart = 0;
static uint8_t        absSlot       = 0;
static PulleysCulture absNewCulture;
static PulleysCulture absOldCultures[NUM_SLOTS];
static float          absStartGb[NUM_SLOTS];  // wanderer gb at absorption trigger

// Max output of the piecewise brightness map (at pos=1.0) — used to land wanderers
// at a known value matching the end of the RESTORE phase
static constexpr float WANDER_PEAK_GB = 0.75f;

// Pending absorption trigger from BLE callback (avoids race with render loop)
static volatile int8_t pendingSlot = -1;
static PulleysCulture  pendingCulture;

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

        // Queue absorption for main loop (avoid race with render)
        uint8_t s = random(NUM_SLOTS);
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

    // LEDs — 8 cols x 32 rows, serpentine
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(255);
    Serial.printf("  LEDs ready — pin %d, %d leds\n", LED_PIN, LED_COUNT);

    // 4 random culture slots using PatternSlot
    randomSeed(esp_random());
    for (uint8_t i = 0; i < NUM_SLOTS; i++) {
        patSlots[i].buffer = leds + i * ROWS_PER_SLOT * COLS;
        patSlots[i].serpentine = true;
        patSlots[i].maxBri = 255;  // shape renderer at full range; wanderer controls brightness
        patSlots[i].init(stationPatternType, ROWS_PER_SLOT, COLS);
        patSlots[i].culture = pulleys::culture_random();
        // Stagger wanderer starting positions so slots aren't in lockstep
        float startPos = 0.2f + (random8() / 255.0f) * 0.5f;
        briWanderers[i].configure(startPos, 3.0f, 2.0f, 0.5f, true, 0.0f, 1.0f);
        char label[8];
        snprintf(label, sizeof(label), "slot%d", i);
        pulleys::culture_print(label, patSlots[i].culture);
    }

    // Proximity
    proximity.setZoneChangeCallback(onZoneChange);

    // BLE scanning
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new TravelerCallbacks(), true);
    pScan->setActiveScan(false);
    pScan->setInterval(160);   // 100ms
    pScan->setWindow(80);      // 50ms — matches traveler, leaves CPU for render
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

    // Start absorption sequence from pending BLE trigger
    if (pendingSlot >= 0 && absPhase == ABS_IDLE) {
        absSlot = (uint8_t)pendingSlot;
        absNewCulture = pendingCulture;
        for (uint8_t i = 0; i < NUM_SLOTS; i++) {
            absOldCultures[i] = patSlots[i].culture;
            // Capture current wanderer output so DIM fades from the actual brightness
            float bp = briWanderers[i].pos;
            if      (bp < 0.40f) absStartGb[i] = (bp / 0.40f) * 0.04f;
            else if (bp < 0.90f) absStartGb[i] = 0.04f + ((bp - 0.40f) / 0.50f) * 0.31f;
            else                 absStartGb[i] = 0.35f + ((bp - 0.90f) / 0.10f) * 0.40f;
        }
        absPhase = ABS_DIM;
        absPhaseStart = now;
    }
    pendingSlot = -1;

    // LED pattern update (30 fps)
    if (now - lastLed >= (1000 / LED_FPS)) {
        lastLed = now;
        float t  = now / 1000.0f;
        float dt = 1.0f / LED_FPS;

        if (absPhase != ABS_IDLE) {
            // ── Absorption sequence ──────────────────────────────────────────────
            // All phase boundaries are continuous — see transition table:
            //   DIM    : slot s fades absStartGb[s]*MB → 0           over 0.5s
            //   FLASH  : all ramp 0 → 255 in 0.3s, hold 255          over 3.0s
            //   RESOLVE: absSlot 255→MB, others 255→0                 over 1.0s
            //   RESTORE: absSlot MB→PEAK*MB, others 0→PEAK*MB         over 1.5s
            //   →IDLE  : wanderers reset to pos=1.0 → output=PEAK*MB  (matches RESTORE end)
            float e = (now - absPhaseStart) / 1000.0f;

            if (absPhase == ABS_DIM && e >= 0.5f) {
                for (uint8_t i = 0; i < NUM_SLOTS; i++) patSlots[i].culture = absNewCulture;
                absPhase = ABS_FLASH; absPhaseStart = now; e = 0.0f;
                Serial.println("[ABS] Flash");
            } else if (absPhase == ABS_FLASH && e >= 3.0f) {
                absPhase = ABS_RESOLVE; absPhaseStart = now; e = 0.0f;
                Serial.println("[ABS] Resolve");
            } else if (absPhase == ABS_RESOLVE && e >= 1.0f) {
                for (uint8_t i = 0; i < NUM_SLOTS; i++)
                    if (i != absSlot) patSlots[i].culture = absOldCultures[i];
                absPhase = ABS_RESTORE; absPhaseStart = now; e = 0.0f;
                Serial.println("[ABS] Restore");
            } else if (absPhase == ABS_RESTORE && e >= 1.5f) {
                for (uint8_t i = 0; i < NUM_SLOTS; i++) {
                    briWanderers[i].pos = 1.0f;
                    briWanderers[i].vel = 0.0f;
                    briWanderers[i].acc = 0.0f;
                }
                absPhase = ABS_IDLE;
                Serial.println("[ABS] Done");
            }

            const float MB   = (float)MAX_BRIGHTNESS;
            const float PEAK = WANDER_PEAK_GB * MB;  // 96 — matches wanderer at pos=1.0

            for (uint8_t s = 0; s < NUM_SLOTS; s++) {
                pulleys::pattern_slot_update(patSlots[s], dt, t);
                briWanderers[s].update(dt);

                float scale;
                if (absPhase == ABS_DIM) {
                    scale = absStartGb[s] * MB * (1.0f - e / 0.5f);
                } else if (absPhase == ABS_FLASH) {
                    float ramp = (e < 0.3f) ? e / 0.3f : 1.0f;
                    scale = ramp * 255.0f;
                } else if (absPhase == ABS_RESOLVE) {
                    float t01 = e / 1.0f;
                    scale = (s == absSlot) ? (255.0f - (255.0f - MB) * t01)
                                           : (255.0f * (1.0f - t01));
                } else {  // ABS_RESTORE
                    float t01 = e / 1.5f;
                    scale = (s == absSlot) ? (MB + (PEAK - MB) * t01)
                                           : (PEAK * t01);
                }

                uint8_t sc = (uint8_t)scale;
                CRGB* buf = patSlots[s].buffer;
                for (uint16_t j = 0; j < ROWS_PER_SLOT * COLS; j++) buf[j].nscale8(sc);
            }
        } else {
            // ── Normal rendering with per-slot brightness wanderers ──────────────
            for (uint8_t s = 0; s < NUM_SLOTS; s++) {
                pulleys::pattern_slot_update(patSlots[s], dt, t);

                briWanderers[s].update(dt);
                float bp = briWanderers[s].pos;
                float gb;
                if      (bp < 0.40f) gb = (bp / 0.40f) * 0.04f;
                else if (bp < 0.90f) gb = 0.04f + ((bp - 0.40f) / 0.50f) * 0.31f;
                else                 gb = 0.35f + ((bp - 0.90f) / 0.10f) * 0.40f;
                uint8_t briScale = (uint8_t)(gb * MAX_BRIGHTNESS);

                CRGB* buf = patSlots[s].buffer;
                for (uint16_t j = 0; j < ROWS_PER_SLOT * COLS; j++) buf[j].nscale8(briScale);
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
