#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_imu.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <pulleys_detect.h>
#include <pulleys_culture.h>
#include <pulleys_patterns.h>
#include <pulleys_channel.h>
#include <pulleys_panel.h>
#include <pulleys_wave.h>

// ── Sensor — rope-mounted motion detector ─────────────────────────────────────
// Runs the traveler board (ESP32-S3 + QMI8658). No sleep: mains-class battery.
// Detects a motion bout (rotation or linear), broadcasts an EVENT to the mesh.
//
// Per-unit config lives in NVS (namespace "sensor"): channel + mode + threshold.
// Set it over serial at boot — see handleSerial() below.
//
// Two LED surfaces on the one output, in this order along the strip:
//
//   8x8 panel   the channel's pattern — pixel for pixel what that channel's
//               block shows on a Screen, which is how a rope and its slot in
//               the array are visibly the same thing
//   16x2 bar    a pot of colour bubbling around the same channel hue (16, skip
//               1, 16 — see lib/pulleys_panel)
//
// Both are the channel's colour: the panel's pattern is built from it, and the
// bar's hue arc is centred on it. One rope reads as one colour from either
// surface, and a rope that is re-channelled changes both at once.

#ifndef LED_PIN
  #define LED_PIN   14
#endif
#ifndef LED_COUNT
  #define LED_COUNT 97
#endif

// Wire order of the chain on the output. WS2812B is normally GRB, which is what
// the 16x2 bar wants, so the controller is told that and the bar needs nothing
// further. Get it wrong and hues are permuted rather than absent -- channel 11's
// purple comes out green when R and G are swapped, which reads as "wrong
// channel" rather than as "wrong wiring". 'p' on serial shows solid primaries
// to settle it: see handleSerial.
#ifndef LED_COLOR_ORDER
  #define LED_COLOR_ORDER GRB
#endif

// The 8x8 panel on this hardware is a plain RGB chip, where the bar after it is
// standard GRB. Measured with the 'p' probe rather than inferred: sending a
// logical RED (byte 1 under a GRB chain) lit the panel green and the bar red,
// sending GREEN (byte 0) lit the panel red and the bar green, and BLUE (byte 2)
// lit both blue. So the panel reads bytes as R, G, B and the bar as G, R, B.
//
// One controller emits one order for the whole chain, so the surface that
// disagrees is permuted in the buffer instead. Against a GRB chain, an RGB chip
// is red and green exchanged.
#ifndef GRID_CHANNEL_SWAP
  #define GRID_CHANNEL_SWAP pulleys::SWAP_RG
#endif

// Where each surface starts on the strip. The bar follows the panel, so its
// origin is the panel's length -- say it that way rather than writing 64, and
// a third surface chained on later needs no arithmetic done by hand.
static constexpr uint16_t GRID_ORIGIN = 0;
static constexpr uint16_t BAR_ORIGIN  = GRID_ORIGIN + pulleys::PANEL_8X8_LEDS;
static_assert(LED_COUNT >= BAR_ORIGIN + pulleys::PANEL_16X2_LEDS,
              "LED_COUNT must cover the 8x8 panel and the 16x2 bar after it");

// For printing a -D token (LED_COLOR_ORDER) rather than its expansion.
#define _STRINGIFY(x) #x
#define STRINGIFY(x)  _STRINGIFY(x)

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
#define IDLE_BRIGHTNESS    10     // low — a presence check, not a display; 30% down from 14

// Breath: dark for a long stretch, swell, brief hold at full, fade back.
#define IDLE_OFF_MS   10000
#define IDLE_RISE_MS   3000
#define IDLE_HOLD_MS   1000
#define IDLE_FALL_MS   3000
#define IDLE_CYCLE_MS (IDLE_OFF_MS + IDLE_RISE_MS + IDLE_HOLD_MS + IDLE_FALL_MS)

// The bar sleeps as an ember in its middle four columns, not as the whole bar
// dimmed. Dimming the whole thing to the breath measured correct -- 0 to 14 of
// 255, in step with the candle -- and read as nothing: 32 pixels at 5% is a
// faint wash, where the candle's nine pixels against 55 dark ones reads as a
// thing breathing. Same light, concentrated instead of spread.
#define IDLE_BAR_X0   6
#define IDLE_BAR_X1   9

// Flicker: how deep each pixel's own wander goes, and how fast it wanders.
// FLICKER_FLOOR is the dimmest a pixel gets as a fraction of the breath, so a
// candle never fully gutters out mid-swell.
#define FLICKER_FLOOR  0.05f
#define FLICKER_SPEED  9        // inoise8 steps per ms/16 — higher is twitchier
#define ACTIVE_BRIGHTNESS  54     // pattern while awake; 30% down from 77, see note below

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
// The panel keeps going through PatternSlot rather than through a Panel: that
// match with the Screen's block is the point of it, and both ends get their
// mapping from channel_slot_init.
static pulleys::PatternSlot patSlot;

// The grid is drawn through patSlot, but it is still a surface: this is what
// carries its wiring -- the serpentine run and the channel swap above -- for
// the output-stage fixup in showPixels().
static pulleys::Panel      gridPanel;

// The bar is the other way round — its own surface, its own pattern, sharing
// only the channel hue. pulleys_panel holds the fold, pulleys_wave the motion.
static pulleys::Panel      barPanel;
static pulleys::WaveParams barCfg;
static pulleys::WaveState  barState;

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

    // Which rope, read which way: the one fact that tells two sensors apart in
    // a survey. Set here so it follows every change, not just the boot value.
    char detail[16];
    snprintf(detail, sizeof(detail), "ch%d %s", myChannel,
             pulleys::sensor_mode_name(myMode));
    pulleys::census_set_detail(detail);
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

// Point both surfaces at the current channel. Call after any channel change --
// 'c7' on serial has to move the bar as well as the panel, or a re-channelled
// rope ends up showing two different ropes' colours.
static void applyChannelVisual() {
    pulleys::channel_slot_init(patSlot, myChannel, leds + GRID_ORIGIN,
                               8, 8, /*serpentine=*/false);
    gridPanel = pulleys::panel_8x8(leds, GRID_ORIGIN, /*serpentine=*/false,
                                   GRID_CHANNEL_SWAP);

    barPanel = pulleys::panel_16x2(leds, BAR_ORIGIN);
    // The channel hue is the centre of the arc the pot bubbles within, which is
    // the same hue the panel's pattern is built from (pulleys_channel). Full
    // brightness here: the envelope below does all the scaling, exactly as it
    // does for the panel.
    barCfg            = pulleys::WaveParams{};
    barCfg.centerHue  = pulleys::channel_hue(myChannel);
    barCfg.maxBri     = 255;
    pulleys::wave_init(barState, barCfg);
}

// Every show goes through here, because the grid's channel swap has to be
// applied once to a finished frame -- applying it twice cancels out, and
// applying it inside a renderer gets overwritten by the next one to draw.
static void showPixels() {
    pulleys::panel_fix_order(gridPanel);
    pulleys::panel_fix_order(barPanel);
    FastLED.show();
}

// ── The bar ───────────────────────────────────────────────────────────────────
// Rendered every frame, lit by the same envelope as the panel. It has to run
// even while dark: the pot integrates its own phases, so a bar that stopped at
// rest would resume mid-stride and jump the moment someone pulls the rope.
//
// At rest it carries the idle breath rather than going fully black, for the
// reason the candle exists on the panel -- a strip that is properly dark is
// indistinguishable from a strip that was never plugged in.
static uint8_t barBri   = 0;  // last level the bar's pattern was lit at
static uint8_t barEmber = 0;  // ...and its sleeping ember, for the status log

static void renderBar(float dt, uint32_t now) {
    pulleys::wave_render(barPanel, barCfg, barState, dt);

    // The pot is lit by the detection envelope alone, so at rest the bar goes
    // dark exactly as the panel's pattern does.
    barBri = (uint8_t)(ledEnv * ACTIVE_BRIGHTNESS);
    for (uint16_t i = 0; i < pulleys::PANEL_16X2_LEDS; i++)
        leds[BAR_ORIGIN + i].nscale8(barBri);

    // ...and the ember takes over underneath it, on the same breath and the
    // same flicker as the candle, so one creature is asleep on both surfaces
    // rather than two things idling differently. Additive and cross-faded
    // against the pattern for the same reason the candle is.
    float breath = IDLE_BRIGHTNESS * (1.0f - ledEnv) * idleBreath(now);
    if (!SENSOR_IDLE_CANDLE || breath <= 0.0f) return;

    CRGB c = pulleys::channel_color(myChannel);
    uint8_t k = 0;
    for (uint8_t y = 0; y < barPanel.rows; y++) {
        for (uint8_t x = IDLE_BAR_X0; x <= IDLE_BAR_X1; x++, k++) {
            CRGB px = c;
            // Offset past the candle's streams so the two surfaces flicker
            // independently -- in step, they read as one wired-together blink.
            px.nscale8((uint8_t)(breath * idleFlicker(IDLE_PIXEL_COUNT + k, now)));
            leds[pulleys::panel_index(barPanel, x, y)] += px;
        }
    }
    barEmber = (uint8_t)breath;
}

// ── Colour-order probe ────────────────────────────────────────────────────────
// Cycles pure red, green, blue across both surfaces, one every PROBE_STEP_MS,
// so the wire order can be read off the strip instead of inferred from a
// pattern. A pattern is a bad instrument for this: channel 11's purple is
// (155, 18, 115), where red and blue are close enough that a permuted version
// of it and the real thing are both describable as "blue".
//
// Primaries have no such problem, and each one answers for exactly one byte
// position: with the chain declared GRB, a logical RED lands in byte 1, GREEN
// in byte 0 and BLUE in byte 2. So the colour the strip shows for each primary
// names the hardware channel at that position, and the three answers together
// give the chip's order outright.
//
// It deliberately bypasses panel_fix_order: the point is the raw wiring, and a
// fixup in the way would hide the thing being measured. Every pixel is lit,
// the fold included -- nothing is excluded from a wiring test for looks.
#define PROBE_STEP_MS 1500
static const CRGB PROBE_COLOR[3] = { CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255) };
static const char* PROBE_NAME[3] = { "RED", "GREEN", "BLUE" };
static bool     probeOn   = false;
static uint8_t  probeIdx  = 0;
static uint32_t probeNext = 0;

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
//   c<0-15> channel | m0/m1 mode | r<deg> threshold | t test event
//   p       colour-order probe: cycles raw RED, GREEN, BLUE on both surfaces
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
    // Long enough for the OTA intake header, which is the longest line any
    // role accepts: "OTA-IMG env=… size=… md5=<32 hex> id=…" runs past 70
    // characters, and a short buffer truncates it silently rather than failing.
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;   // "?" → PULLEYS-ID line
            if (pulleys::ota_handle_serial(buf)) continue;   // "OTA-SLOT" / "OTA-SERVE"
            if (pulleys::census_handle_serial(buf)) continue; // "CENSUS" → who is out there
            if (buf[0] == 'O') {                 // "O" → become the OTA host
                pulleys::ota_host_begin();
                continue;
            }
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
            } else if (buf[0] == 'p') {          // "p" → the colour-order probe
                probeOn = !probeOn;
                probeIdx = 0;
                probeNext = 0;
                Serial.printf(probeOn
                    ? "  probe on — raw %s order, cycling RED, GREEN, BLUE every"
                      " %dms. What the strip shows for each names that byte's"
                      " hardware channel.\n"
                    : "  probe off — back to the pattern (%s, %dms)\n",
                    STRINGIFY(LED_COLOR_ORDER), PROBE_STEP_MS);
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

    FastLED.addLeds<WS2812B, LED_PIN, LED_COLOR_ORDER>(leds, LED_COUNT);
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
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::mesh_on_event(onMeshEvent);

    applyChannelVisual();

    // Boot: flash the channel color three times so the install crew can verify.
    // An unassigned board flashes white instead — see the UNASSIGNED note above.
    FastLED.setBrightness(255);   // brightness is applied per-pixel from here on
    for (int i = 0; i < 3; i++) {
        CRGB boot = (chanSource == CHAN_LISTED) ? pulleys::channel_color(myChannel)
                                                : CRGB::White;
        boot.nscale8(28);         // 30% down from 40, with the rest of the panel
        fill_solid(leds, LED_COUNT, boot);
        showPixels();
        delay(120);
        fill_solid(leds, LED_COUNT, CRGB::Black);
        showPixels();
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
    pulleys::census_poll();

    // OTA owns the board while it runs: the radio wants the airtime, and
    // FastLED.show() turns interrupts off for long enough to cost packets.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

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

        // The probe owns the whole strip while it runs: it is answering a
        // question about the wiring, and a pattern underneath it would only
        // make the answer harder to read. It goes through the same fixup as
        // everything else, so the two surfaces agreeing on a primary is the
        // pass condition -- if one of them disagrees, its swap is wrong.
        if (probeOn) {
            if ((int32_t)(now - probeNext) >= 0) {
                probeNext = now + PROBE_STEP_MS;
                Serial.printf("  probe: sending %s\n", PROBE_NAME[probeIdx]);
                probeIdx = (probeIdx + 1) % 3;
            }
            uint8_t shown = (probeIdx + 2) % 3;    // the one currently up
            CRGB c = PROBE_COLOR[shown];
            c.nscale8(ACTIVE_BRIGHTNESS);
            fill_solid(leds, LED_COUNT, c);
            FastLED.show();                        // raw: no panel_fix_order
            return;
        }

        // The panel. Scoped to its own 64 pixels now that the bar lives on the
        // same strip -- clearing or scaling LED_COUNT would reach into it.
        if (ledEnv > 0.002f) {
            // Awake: the channel's own pattern, the same one this channel's
            // block shows on a Screen.
            pulleys::pattern_slot_update(patSlot, dt, pulleys::mesh_now_secs());
            uint8_t bri = (uint8_t)(ledEnv * ACTIVE_BRIGHTNESS);
            for (uint16_t i = 0; i < pulleys::PANEL_8X8_LEDS; i++)
                leds[GRID_ORIGIN + i].nscale8(bri);
        } else {
            fill_solid(leds + GRID_ORIGIN, pulleys::PANEL_8X8_LEDS, CRGB::Black);
        }

        renderBar(dt, now);

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
                    leds[GRID_ORIGIN + IDLE_PIXEL_IDX[i]] += p;
                }
            }
        }
        showPixels();
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
        Serial.printf("  [LED] env=%.3f breath=%.2f  bar=%u/%u ember=%u\n",
                      ledEnv, idleBreath(now), barBri, ACTIVE_BRIGHTNESS, barEmber);
        Serial.printf("  [SYNC] clock=%s meshNow=%lums\n",
                      pulleys::mesh_clock_locked() ? "locked" : "free",
                      (unsigned long)pulleys::mesh_now());
        pulleys::mesh_print_stats();
    }
}
