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
// nothing at rest and lights up only while it is detecting and sending.
//
// The idle state is a 3x3 candle in the middle of the panel: an unlit rope can
// still be told apart from a dead board, but it reads as something quietly
// alive rather than as a status LED. Set SENSOR_IDLE_CANDLE to 0 for a panel
// that is properly dark at rest.
//
// Two independent motions make it look like a flame rather than a blinking
// block. The breath is the whole candle swelling and fading on a long cycle;
// the flicker is each of the nine pixels wandering on its own. Neither is
// phase-locked to the mesh clock the way the active pattern is — a row of
// sensors pulsing in unison reads as one machine blinking, where independent
// phases read as several quiet things idling. Each board picks its breath
// offset at boot, and each pixel its own flicker stream.
//
// ACTIVE_BRIGHTNESS is deliberately well short of full. The panel is at arm's
// length from whoever just pulled the rope, in the dark, on eyes that have
// been adapted for a while — at 190 it is a lamp rather than a pattern, and
// the shape washes out into a single bright block. Raise it only if the piece
// ends up somewhere with real ambient light to compete with.
#define SENSOR_IDLE_CANDLE 1
#define IDLE_BRIGHTNESS    14     // low — a presence check, not a display

// Breath: dark for a long stretch, swell, brief hold at full, fade back.
#define IDLE_OFF_MS   10000
#define IDLE_RISE_MS   3000
#define IDLE_HOLD_MS   1000
#define IDLE_FALL_MS   3000
#define IDLE_CYCLE_MS (IDLE_OFF_MS + IDLE_RISE_MS + IDLE_HOLD_MS + IDLE_FALL_MS)

// Flicker: how deep each pixel's own wander goes, and how fast it wanders.
// FLICKER_FLOOR is the dimmest a pixel gets as a fraction of the breath, so a
// candle never fully gutters out mid-swell.
#define FLICKER_FLOOR  0.05f
#define FLICKER_SPEED  9        // inoise8 steps per ms/16 — higher is twitchier
#define ACTIVE_BRIGHTNESS  77     // 30% of full — pattern while awake; see note below

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
static uint8_t  myMode    = pulleys::SENSOR_MODE_LINEAR;
static float    myRotDeg  = 180.0f;
static uint32_t holdUntil = 0;    // envelope stays up until this moment
static uint32_t idlePhase = 0;    // per-board offset into the breath cycle
static float    ledEnv    = 0.0f; // 0 = dark/idle pixels, 1 = full pattern
static uint32_t localCount = 0;
static uint32_t heardCount = 0;
// Where myChannel came from, in descending order of authority. Kept apart
// because they mean different things to whoever is holding the board: LISTED is
// settled, NVS is someone's field fix, HASH is a guess that works.
enum ChanSource : uint8_t { CHAN_LISTED, CHAN_NVS, CHAN_HASH, CHAN_DEFAULT };
static ChanSource chanSource = CHAN_HASH;
static ChanSource modeSource = CHAN_DEFAULT;   // no hash fallback for mode

// The install map mirrors these rather than including the radio header, so
// check here — where both are visible — that the two have not drifted apart.
static_assert(pulleys::ROT == pulleys::SENSOR_MODE_ROTATION, "ROT/SENSOR_MODE_ROTATION disagree");
static_assert(pulleys::LIN == pulleys::SENSOR_MODE_LINEAR,   "LIN/SENSOR_MODE_LINEAR disagree");

// ── Config persistence ────────────────────────────────────────────────────────
static void loadConfig() {
    Preferences p;
    p.begin(NVS_NS, true);
    // 0xFF, not 0, as the default: a board that has never been told its channel
    // must be distinguishable from one deliberately put on channel 0. With 0 as
    // the default they look identical, and a whole crate of freshly flashed
    // boards silently piles onto channel 0.
    uint8_t stored     = p.getUChar("ch",   0xFF);
    uint8_t storedMode = p.getUChar("mode", 0xFF);
    myRotDeg  = p.getFloat("rot",  180.0f);
    p.end();
    if (stored <= 15) { myChannel = stored; chanSource = CHAN_NVS; }
    // Same sentinel reasoning as the channel above: with LINEAR as the read
    // default, a board nobody has configured is indistinguishable from one
    // deliberately set to linear, and the boot line then claims someone chose
    // it. Report what is actually known.
    if (storedMode <= pulleys::LIN) { myMode = storedMode; modeSource = CHAN_NVS; }
}

static void saveConfig() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar("ch",   myChannel);
    p.putUChar("mode", myMode);
    p.putFloat("rot",  myRotDeg);
    p.end();
}

// The repo's channel table wins over the board's stored value. NVS is only
// consulted for a board the table does not name — see pulleys_channel.h. Must
// run after identity_init(), since the lookup is by device ID.
static void applyChannelAssignment() {
    uint16_t id     = pulleys::identity_id();
    int8_t assigned = pulleys::channel_for_device(id);
    // Mode is taken from the same row, and unlike the channel it has no
    // sensible fallback: guessing how a rope is rigged would be worse than
    // keeping whatever was set by hand, so an unlisted board keeps its NVS mode.
    int8_t listedMode = pulleys::mode_for_device(id);
    if (listedMode >= 0) {
        myMode     = (uint8_t)listedMode;
        modeSource = CHAN_LISTED;
    }

    if (assigned >= 0) {
        myChannel  = (uint8_t)assigned;
        chanSource = CHAN_LISTED;
        return;
    }
    // Not listed, and nobody has set one over serial either: derive one rather
    // than default. A crate of boards all defaulting to the same channel is the
    // failure that hides itself; twelve boards scattered over twelve channels is
    // at least visibly wrong when two collide.
    if (chanSource == CHAN_HASH) myChannel = pulleys::channel_from_id(id);
}

static void applyConfig() {
    pulleys::DetectConfig c;
    c.mode            = myMode;
    c.rotThresholdDeg = myRotDeg;
    detector.init(c);
}

// Centre 3x3 of the 8x8 panel — the idle candle. Rows 3-5, cols 3-5 of a
// non-serpentine 8x8, which keeps the old centre pixels inside the block.
static const uint8_t IDLE_PIXEL_IDX[9] = {
    27, 28, 29,
    35, 36, 37,
    43, 44, 45,
};
static constexpr uint8_t IDLE_PIXEL_COUNT =
    sizeof(IDLE_PIXEL_IDX) / sizeof(IDLE_PIXEL_IDX[0]);

// 0 at rest, 1 at the top of the swell. Linear ramps: the eye reads the long
// dark gap and the slow rise, not the curve shape, so easing would not earn
// its complexity here.
static float idleBreath(uint32_t now) {
    uint32_t t = (now + idlePhase) % IDLE_CYCLE_MS;
    if (t < IDLE_OFF_MS)  return 0.0f;
    t -= IDLE_OFF_MS;
    if (t < IDLE_RISE_MS) return (float)t / IDLE_RISE_MS;
    t -= IDLE_RISE_MS;
    if (t < IDLE_HOLD_MS) return 1.0f;
    t -= IDLE_HOLD_MS;
    return 1.0f - (float)t / IDLE_FALL_MS;
}

// Per-pixel flicker in FLICKER_FLOOR..1. Perlin noise rather than random8 so
// each pixel wanders smoothly instead of buzzing; the large y offset keeps the
// nine streams from correlating with one another.
static float idleFlicker(uint8_t i, uint32_t now) {
    uint8_t n = inoise8((uint16_t)(now * FLICKER_SPEED / 16), (uint16_t)(i * 977));
    return FLICKER_FLOOR + (1.0f - FLICKER_FLOOR) * (n / 255.0f);
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
    Serial.printf("  CONFIG  channel=%d (%s)  mode=%s (%s)  rotThreshold=%.0f deg\n",
                  myChannel, chanSource == CHAN_LISTED ? "listed"
                           : chanSource == CHAN_NVS    ? "set over serial"
                                                       : "UNLISTED — hashed from ID",
                  myMode == pulleys::SENSOR_MODE_ROTATION ? "ROTATION" : "LINEAR",
                  modeSource == CHAN_LISTED ? "listed"
                           : modeSource == CHAN_NVS ? "set over serial"
                                                    : "nobody set one — default",
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
                if (v >= 0 && v <= 15) {
                    myChannel = v; chanSource = CHAN_NVS;
                    saveConfig(); applyChannelVisual();
                    Serial.printf("  → add to CHANNEL_ASSIGNMENT: { 0x%04X, %d },\n",
                                  pulleys::identity_id(), v);
                    if (chanSource == CHAN_LISTED)
                        Serial.printf("  ! this board is listed in CHANNEL_ASSIGNMENT;"
                                      " the table wins again at next boot\n");
                }
            } else if (buf[0] == 'm') {          // "m0" rotation, "m1" linear
                int v = atoi(buf + 1);
                if (v == 0 || v == 1) {
                    myMode = v; modeSource = CHAN_NVS;
                    saveConfig(); applyConfig();
                    if (pulleys::mode_for_device(pulleys::identity_id()) >= 0)
                        Serial.printf("  ! this board's mode is listed;"
                                      " the table wins again at next boot\n");
                }
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
    applyChannelAssignment();
    printConfig();
    Serial.println("  serial: c<0-15> channel | m0/m1 mode | r<deg> | t test");

    if (!imu.init6(11, 12)) {
        Serial.println("  [IMU] 6-axis init FAILED — no detection possible");
    }
    applyConfig();

    idlePhase = esp_random() % IDLE_CYCLE_MS;

    pulleys::mesh_init(pulleys::MESH_ORIGIN_SENSOR, pulleys::identity_id());
    pulleys::mesh_on_event(onMeshEvent);

    applyChannelVisual();

    // Boot: flash the channel color three times so the install crew can verify.
    // An unassigned board flashes white instead — see the UNASSIGNED note above.
    FastLED.setBrightness(255);   // brightness is applied per-pixel from here on
    for (int i = 0; i < 3; i++) {
        CRGB boot = (chanSource == CHAN_LISTED) ? pulleys::channel_color(myChannel)
                                                : CRGB::White;
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

        // An unassigned board does not get to look like a working one. The whole
        // panel breathes white — a colour no channel ever uses — so a crate of
        // freshly flashed boards sorts itself into "set up" and "not set up" at
        // a glance, across a dark room, by someone who is doing three other
        // things. This overrides the pattern entirely.
        if (ledEnv < 1.0f) {
            // Presence pixels cross-fade against the pattern, so the hand-off in
            // both directions is smooth rather than a pop.
            CRGB c = pulleys::channel_color(myChannel);
            float breath = IDLE_BRIGHTNESS * (1.0f - ledEnv) * idleBreath(now);
            if (SENSOR_IDLE_CANDLE && breath > 0.0f) {
                for (uint8_t i = 0; i < IDLE_PIXEL_COUNT; i++) {
                    CRGB p = c;
                    p.nscale8((uint8_t)(breath * idleFlicker(i, now)));
                    leds[IDLE_PIXEL_IDX[i]] += p;
                }
            }
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
