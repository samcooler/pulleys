#include <Arduino.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>

// ── Bridge — mesh events out to a Raspberry Pi ────────────────────────────────
// Joins the mesh like any other node: it relays, answers census, and takes OTA.
// What it adds is a second output. Every deduped sensor EVENT is written to the
// Pi as one text line on UART1:
//
//   EV <channel> <mode> <magnitude> <originId hex> <flags>\n
//
// e.g. "EV 3 1 90 A855 0". Fields are space-separated and parsed by position;
// anything new is appended. The Pi's own lines are echoed to the console.
//
// The link is UART1 rather than Serial on purpose: UART0 is the console and the
// channel whoami, census and OTA intake arrive on, and mixing the Pi's traffic
// into it would put boot banners in front of whatever is reading the Pi.

#ifndef BRIDGE_TX_PIN
  #define BRIDGE_TX_PIN 19
#endif
#ifndef BRIDGE_RX_PIN
  #define BRIDGE_RX_PIN 18
#endif
#ifndef BRIDGE_BAUD
  #define BRIDGE_BAUD   115200
#endif

#define QUEUE_LEN 32   // events waiting for the UART; a burst of sensors fits

struct QueuedEvent {
    uint8_t  channel, mode, magnitude, flags;
    uint16_t originId;
};
static QueuedEvent queue[QUEUE_LEN];
static uint8_t     qHead = 0, qTail = 0;

static uint32_t forwarded = 0;
static uint32_t dropped   = 0;

// The mesh callback only queues. The UART write happens in loop(), so a slow or
// unplugged Pi can never stall the radio path.
static void onMeshEvent(const pulleys::MeshEvent& ev, bool relayed) {
    uint8_t next = (qHead + 1) % QUEUE_LEN;
    if (next == qTail) { dropped++; return; }
    queue[qHead] = { ev.channel, ev.mode, ev.magnitude, ev.flags, ev.originId };
    qHead = next;
    Serial.printf("★ ch%-2d from 0x%04X %s mag=%3d\n", ev.channel, ev.originId,
                  relayed ? "(relayed)" : "(direct) ", ev.magnitude);
}

static void drainQueue() {
    while (qTail != qHead) {
        // Leave the event queued if the UART would block; try again next loop.
        if (Serial1.availableForWrite() < 32) return;
        const QueuedEvent& q = queue[qTail];
        Serial1.printf("EV %u %u %u %04X %u\n", q.channel, q.mode, q.magnitude,
                       q.originId, q.flags);
        qTail = (qTail + 1) % QUEUE_LEN;
        forwarded++;
    }
}

// Lines from the Pi (acks, errors) go to the console so they can be read.
static void pollPi() {
    static char buf[96];
    static uint8_t len = 0;
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n' || c == '\r') {
            if (len) { buf[len] = 0; Serial.printf("  [PI] %s\n", buf); }
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ── Serial console ────────────────────────────────────────────────────────────
//   e<ch>  inject a fake event on that channel, as if the mesh had delivered it
static void handleSerial() {
    // Long enough for the OTA intake header, the longest line any role accepts.
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;
            if (pulleys::ota_handle_serial(buf)) continue;
            if (pulleys::census_handle_serial(buf)) continue;
            if (buf[0] == 'O') {
                pulleys::ota_host_begin();
            } else if (buf[0] == 'e') {
                pulleys::MeshEvent ev = {};
                ev.channel   = (uint8_t)atoi(buf + 1);
                ev.mode      = pulleys::SENSOR_MODE_LINEAR;
                ev.magnitude = 90;
                ev.originId  = pulleys::identity_id();
                ev.ttl       = pulleys::MESH_TTL_START;
                onMeshEvent(ev, false);
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial1.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);

    pulleys::identity_init(PULLEYS_TYPE_BRIDGE);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Bridge  %s\n", pulleys::identity_name());
    Serial.printf("  Pi link: UART1 tx=%d rx=%d @ %d\n",
                  BRIDGE_TX_PIN, BRIDGE_RX_PIN, BRIDGE_BAUD);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    randomSeed(esp_random());
    pulleys::mesh_init(pulleys::MESH_ORIGIN_BRIDGE, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    pulleys::mesh_on_event(onMeshEvent);

    pulleys::census_set_detail("uart1");
    pulleys::whoami_reply();
    Serial.println("Bridge ready — forwarding mesh events to the Pi.\n");
}

void loop() {
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    pulleys::census_poll();

    // OTA owns the board while it runs: the radio wants the airtime.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

    handleSerial();
    drainQueue();
    pollPi();

    if (now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── forwarded=%lu dropped=%lu\n",
                      (unsigned long)forwarded, (unsigned long)dropped);
        pulleys::mesh_print_stats();
    }
}
