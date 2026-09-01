#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
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
#define NVS_NS          "sensor"

// ── Idle / active look ────────────────────────────────────────────────────────
// The installed piece is dark until someone plays with it, so a sensor shows
// nothing at rest and lights fully only while it is detecting and sending.
//
// SENSOR_IDLE_PIXELS is a debugging affordance: a few pixels stay lit in the
// channel colour so an unlit rope can be told apart from a dead board. Set it
// to 0 for the installed behaviour — properly dark at rest.
#define SENSOR_IDLE_PIXELS 4
#define IDLE_BRIGHTNESS    14     // low — a presence check, not a display
#define ACTIVE_BRIGHTNESS  190    // full pattern while awake

// Envelope around a detection: snap up, hold, drift back down. Re-triggering
// only pushes the hold out — the envelope keeps rising from wherever it is and
// the pattern, being phase-locked to the mesh clock, never restarts. So a rope
// worked repeatedly just stays lit and moving rather than stuttering.
#define LED_ATTACK_MS   140
#define LED_HOLD_MS     4000
#define LED_RELEASE_MS  900

static CRGB leds[LED_COUNT];
static pulleys::IMU      imu;
static pulleys::Detector detector;

// The sensor renders its own channel's pattern — the same one that channel's
// block shows on a Screen, so a rope and its slot in the array visibly match.
static pulleys::PatternSlot patSlot;

static uint8_t  myChannel = 0;
static uint8_t  myMode    = pulleys::SENSOR_MODE_ROTATION;
static float    myRotDeg  = 180.0f;
static uint32_t holdUntil = 0;    // envelope stays up until this moment
static float    ledEnv    = 0.0f; // 0 = dark/idle pixels, 1 = full pattern
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

// Centre 2x2 of the 8x8 panel — the idle presence pixels.
static const uint8_t IDLE_PIXEL_IDX[4] = { 27, 28, 35, 36 };

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
            if (pulleys::whoami_handle(buf)) continue;   // "?" → PULLEYS-ID line
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
                holdUntil = millis() + LED_HOLD_MS;
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
    FastLED.setBrightness(255);   // brightness is applied per-pixel from here on
    for (int i = 0; i < 3; i++) {
        CRGB boot = pulleys::channel_color(myChannel);
        boot.nscale8(40);
        fill_solid(leds, LED_COUNT, boot);
        FastLED.show();
        delay(120);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        FastLED.show();
        delay(120);
    }
    pulleys::whoami_reply();
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
                holdUntil = now + LED_HOLD_MS;
                pulleys::mesh_send_event(myChannel, myMode, mag, 0);
                Serial.printf("★ DETECT ch%-2d %s mag=%d  (#%lu)\n",
                              myChannel,
                              myMode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin",
                              mag, localCount);
            }
        }
    }

    // LED: dark at rest, full channel pattern while awake
    if (now - lastLed >= (1000 / LED_FPS)) {
        float dt = (now - lastLed) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        lastLed = now;

        // Envelope: rise fast toward a detection, fall slowly away from it.
        bool up = (int32_t)(holdUntil - now) > 0;
        ledEnv += up ? (dt * 1000.0f / LED_ATTACK_MS)
                     : -(dt * 1000.0f / LED_RELEASE_MS);
        if (ledEnv > 1.0f) ledEnv = 1.0f;
        if (ledEnv < 0.0f) ledEnv = 0.0f;

        if (ledEnv > 0.002f) {
            // Awake: the channel's own pattern, the same one this channel's
            // block shows on a Screen.
            pulleys::pattern_slot_update(patSlot, dt, pulleys::mesh_now_secs());
            uint8_t bri = (uint8_t)(ledEnv * ACTIVE_BRIGHTNESS);
            for (uint16_t i = 0; i < LED_COUNT; i++) leds[i].nscale8(bri);
        } else {
            fill_solid(leds, LED_COUNT, CRGB::Black);
        }

        // Presence pixels cross-fade against the pattern, so the hand-off in
        // both directions is smooth rather than a pop.
        if (ledEnv < 1.0f) {
            CRGB c = pulleys::channel_color(myChannel);
            c.nscale8((uint8_t)(IDLE_BRIGHTNESS * (1.0f - ledEnv)));
            for (uint8_t i = 0; i < SENSOR_IDLE_PIXELS && i < 4; i++)
                leds[IDLE_PIXEL_IDX[i]] += c;
        }
        FastLED.show();
    }

    // 2 Hz status
    if (now - lastLog >= 2000) {
        lastLog = now;
        static const char* stNames[] = { "IDLE", "ACTIVE", "REFRAC" };
        if (myMode == pulleys::SENSOR_MODE_LINEAR) {
            // The learned primary axis only converges with real pulls, so show
            // it: a drifting axis means the sensor has not settled yet.
            float ax, ay, az;
            detector.axis(ax, ay, az);
            Serial.printf("[%s] ch%-2d impulse=%.3f resid=%.3f axis=[%+.2f %+.2f %+.2f]  tx=%lu rx=%lu\n",
                          stNames[detector.state()], myChannel,
                          detector.measure(), detector.lastResid(),
                          ax, ay, az, localCount, heardCount);
        } else {
            Serial.printf("[%s] ch%-2d measure=%.1f rate=%.0f resid=%.3f  tx=%lu rx=%lu\n",
                          stNames[detector.state()], myChannel,
                          detector.measure(), detector.lastRate(), detector.lastResid(),
                          localCount, heardCount);
        }
        Serial.printf("  [SYNC] clock=%s meshNow=%lums\n",
                      pulleys::mesh_clock_locked() ? "locked" : "free",
                      (unsigned long)pulleys::mesh_now());
        pulleys::mesh_print_stats();
    }
}
