#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_identity.h>
#include <pulleys_protocol.h>
#include <pulleys_imu.h>
#include <pulleys_mesh.h>
#include <pulleys_detect.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_channel.h>

// ── Sensor — rope-mounted motion detector ─────────────────────────────────────
// Runs the traveler board (ESP32-S3 + QMI8658). No sleep: mains-class battery.
// Detects a motion bout (rotation or linear), broadcasts an EVENT to the mesh.
//
// Per-unit config lives in NVS (namespace "sensor"): channel + mode + threshold.
// Set it over serial at boot — see handleSerial() below.

#ifndef LED_PIN
  #define LED_PIN   14
#endif
#ifndef LED_COUNT
  #define LED_COUNT 64
#endif

#define IMU_HZ          100
#define IMU_INTERVAL_MS (1000 / IMU_HZ)
#define LED_FPS         30
#define FLASH_MS        600     // confirmation flash on local detection
#define IDLE_BRIGHTNESS 22      // resting brightness of the channel pattern
#define FLASH_BRIGHTNESS 150    // peak at the moment of detection
#define NVS_NS          "sensor"

static CRGB leds[LED_COUNT];
static pulleys::IMU      imu;
static pulleys::Detector detector;

// The sensor renders its own channel's pattern — the same one that channel's
// block shows on a Screen, so a rope and its slot in the array visibly match.
static pulleys::PatternSlot patSlot;

static uint8_t  myChannel = 0;
static uint8_t  myMode    = pulleys::SENSOR_MODE_ROTATION;
static float    myRotDeg  = 180.0f;
static uint32_t flashUntil = 0;
static uint32_t localCount = 0;
static uint32_t heardCount = 0;

// ── Config persistence ────────────────────────────────────────────────────────
static void loadConfig() {
    Preferences p;
    p.begin(NVS_NS, true);
    myChannel = p.getUChar("ch",   0);
    myMode    = p.getUChar("mode", pulleys::SENSOR_MODE_ROTATION);
    myRotDeg  = p.getFloat("rot",  180.0f);
    p.end();
    if (myChannel > 15) myChannel = 0;
}

static void saveConfig() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar("ch",   myChannel);
    p.putUChar("mode", myMode);
    p.putFloat("rot",  myRotDeg);
    p.end();
}

static void applyConfig() {
    pulleys::DetectConfig c;
    c.mode            = myMode;
    c.rotThresholdDeg = myRotDeg;
    detector.init(c);
}

// Point the pattern slot at the current channel. Call after any channel change.
static void applyChannelVisual() {
    pulleys::channel_slot_init(patSlot, myChannel, leds, 8, 8, /*serpentine=*/false);
}

// ── Mesh RX — sensors listen too, so they relay for each other ────────────────
static void onMeshEvent(const pulleys::MeshEvent& ev, bool relayed) {
    if (ev.originId == pulleys::identity_id()) return;
    heardCount++;
    Serial.printf("  [RX%s] ch%-2d from 0x%04X  mode=%s mag=%d ttl=%d\n",
                  relayed ? "*" : " ", ev.channel, ev.originId,
                  ev.mode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin",
                  ev.magnitude, ev.ttl);
}

// ── Serial console — field config without a reflash ───────────────────────────
static void printConfig() {
    Serial.printf("  CONFIG  channel=%d  mode=%s  rotThreshold=%.0f deg\n",
                  myChannel,
                  myMode == pulleys::SENSOR_MODE_ROTATION ? "ROTATION" : "LINEAR",
                  myRotDeg);
}

static void handleSerial() {
    static char buf[32];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (buf[0] == 'c') {                 // "c7" → channel 7
                int v = atoi(buf + 1);
                if (v >= 0 && v <= 15) { myChannel = v; saveConfig(); applyChannelVisual(); }
            } else if (buf[0] == 'm') {          // "m0" rotation, "m1" linear
                int v = atoi(buf + 1);
                if (v == 0 || v == 1) { myMode = v; saveConfig(); applyConfig(); }
            } else if (buf[0] == 'r') {          // "r260" → rotation threshold
                int v = atoi(buf + 1);
                if (v >= 30 && v <= 720) { myRotDeg = v; saveConfig(); applyConfig(); }
            } else if (buf[0] == 't') {          // "t" → fire a test event
                pulleys::mesh_send_event(myChannel, myMode, 90, 0);
                localCount++;
                flashUntil = millis() + FLASH_MS;
                Serial.println("  [TX] test event");
            }
            printConfig();
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1200);

    loadConfig();

    FastLED.addLeds<WS2812B, LED_PIN, RGB>(leds, LED_COUNT);
    FastLED.setBrightness(255);
    fill_solid(leds, LED_COUNT, CRGB::Black);
    FastLED.show();

    pulleys::identity_init(PULLEYS_TYPE_SENSOR);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Sensor  %s\n", pulleys::identity_name());
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    printConfig();
    Serial.println("  serial: c<0-15> channel | m0/m1 mode | r<deg> | t test");

    if (!imu.init6(11, 12)) {
        Serial.println("  [IMU] 6-axis init FAILED — no detection possible");
    }
    applyConfig();

    pulleys::mesh_init(pulleys::MESH_ORIGIN_SENSOR, pulleys::identity_id());
    pulleys::mesh_on_event(onMeshEvent);

    applyChannelVisual();

    // Boot: flash the channel color three times so the install crew can verify
    for (int i = 0; i < 3; i++) {
        fill_solid(leds, LED_COUNT, pulleys::channel_color(myChannel));
        FastLED.setBrightness(40);
        FastLED.show();
        delay(120);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        FastLED.show();
        delay(120);
    }
    Serial.println("Sensor ready.\n");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastImu = 0;
    static uint32_t lastLed = 0;
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    handleSerial();

    // Detection
    if (now - lastImu >= IMU_INTERVAL_MS) {
        float dt = (now - lastImu) / 1000.0f;
        lastImu = now;

        pulleys::AccelData a;
        pulleys::IMU::Gyro g;
        if (imu.read6(a, g)) {
            uint8_t mag = 0;
            if (detector.update(a.x, a.y, a.z, g.x, g.y, g.z, dt, mag)) {
                localCount++;
                flashUntil = now + FLASH_MS;
                pulleys::mesh_send_event(myChannel, myMode, mag, 0);
                Serial.printf("★ DETECT ch%-2d %s mag=%d  (#%lu)\n",
                              myChannel,
                              myMode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin",
                              mag, localCount);
            }
        }
    }

    // LED: the channel's own pattern, resting dim and swelling on detection
    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;

        pulleys::pattern_slot_update(patSlot, dt, pulleys::mesh_now_secs());

        uint8_t bri = IDLE_BRIGHTNESS;
        if (now < flashUntil) {
            float f = (flashUntil - now) / (float)FLASH_MS;
            bri = (uint8_t)(IDLE_BRIGHTNESS + f * (FLASH_BRIGHTNESS - IDLE_BRIGHTNESS));
        }
        FastLED.setBrightness(bri);
        FastLED.show();
    }

    // 2 Hz status
    if (now - lastLog >= 2000) {
        lastLog = now;
        static const char* stNames[] = { "IDLE", "ACTIVE", "REFRAC" };
        Serial.printf("[%s] ch%-2d measure=%.1f rate=%.0f resid=%.3f  tx=%lu rx=%lu\n",
                      stNames[detector.state()], myChannel,
                      detector.measure(), detector.lastRate(), detector.lastResid(),
                      localCount, heardCount);
        Serial.printf("  [SYNC] clock=%s meshNow=%lums\n",
                      pulleys::mesh_clock_locked() ? "locked" : "free",
                      (unsigned long)pulleys::mesh_now());
        pulleys::mesh_print_stats();
    }
}
