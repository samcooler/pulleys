#include <Arduino.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <WebServer.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// ── Bridge — mesh events out to a Raspberry Pi ────────────────────────────────
// Joins the mesh like any other node: it relays and answers census. It also
// serves node firmware from the Overlord over its always-on same-channel AP.
// What it adds is a second output. Every deduped sensor EVENT is written to the
// Pi as one text line on UART1:
//
//   EV <channel> <mode> <magnitude> <originId hex> <flags>\n
//
// e.g. "EV 3 1 90 A855 0". Fields are space-separated and parsed by position;
// anything new is appended. The Pi's own lines are echoed to the console.
//
// The arbiter's game fires cues, which arrive as the cue's name in ASCII and go
// out as
//
//   cue <name>\n
//
// e.g. "cue audio_once_chime". The bridge is transparent: it holds no list of
// cues and checks nothing about the name, it passes on whatever it was given.
//
// The link is UART1 rather than Serial on purpose: UART0 is the console and the
// channel whoami, census and update control arrive on, and mixing the UART traffic
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

static WebServer fwServer(80);
static char fwSha[65] = {};
static char fwBuildHash[65] = {};
static char fwMd5[33] = {};
static char fwEnv[32] = {};
static uint32_t fwSize = 0;
static uint16_t fwTarget = 0;
static bool fwOfferActive = false;
static void drainQueue();

static bool requestFirmwareChunk(uint32_t offset, uint8_t* out, size_t want, size_t& got) {
    Serial1.printf("FW-READ sha256=%s offset=%lu length=%u\n", fwSha,
                   (unsigned long)offset, (unsigned)want);
    char line[640];
    uint32_t start = millis();
    size_t used = 0;
    while (millis() - start < 15000) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '\n' || c == '\r') {
                if (!used) continue;
                line[used] = 0;
                if (strncmp(line, "FW-DATA ", 8) == 0) {
                    char* off = strstr(line, "offset=");
                    char* b64 = strstr(line, "b64=");
                    if (!off || !b64 || !strstr(line, fwSha) ||
                        strtoul(off + 7, nullptr, 10) != offset) return false;
                    b64 += 4;
                    size_t decoded = 0;
                    int rc = mbedtls_base64_decode(out, want, &decoded,
                                                  (const unsigned char*)b64, strlen(b64));
                    got = decoded;
                    return rc == 0 && decoded > 0;
                }
                used = 0;
            } else if (used < sizeof(line) - 1) {
                line[used++] = c;
            }
        }
        pulleys::mesh_poll();
        drainQueue();
        delay(1);
    }
    return false;
}

static void serveFirmware() {
    String requested = fwServer.uri();
    const String prefix = "/api/v1/firmware/";
    if (requested.startsWith(prefix)) requested = requested.substring(prefix.length());
    if (!fwOfferActive || (requested != fwSha && requested != "/firmware.bin") || fwSize == 0) {
        fwServer.send(404, "text/plain", "no matching firmware offer");
        return;
    }
    fwServer.setContentLength(fwSize);
    fwServer.send(200, "application/octet-stream", "");
    WiFiClient client = fwServer.client();
    uint8_t data[384];
    uint32_t offset = 0;
    while (offset < fwSize && client.connected()) {
        size_t want = fwSize - offset;
        if (want > sizeof(data)) want = sizeof(data);
        size_t got = 0;
        if (!requestFirmwareChunk(offset, data, want, got) || got > want) break;
        size_t sent = 0;
        while (sent < got && client.connected()) {
            size_t n = client.write(data + sent, got - sent);
            if (!n) break;
            sent += n;
        }
        if (sent != got) break;
        offset += got;
    }
    Serial.printf("[FW] HTTP served %lu/%lu bytes\n", (unsigned long)offset,
                  (unsigned long)fwSize);
}

static void onFirmwareStatus(const pulleys::MeshFwStatus& status) {
    if (status.originId != fwTarget) return;
    const char* state = status.state == pulleys::FW_OK ? "ok"
                      : status.state == pulleys::FW_FAILED ? "failed"
                      : status.state == pulleys::FW_DOWNLOADING ? "downloading" : "ready";
    Serial1.printf("FW-STATUS id=%04X env=%s state=%s build_hash=%s sha256=%s err=%d\n",
                   status.originId, status.env, state, status.buildHash,
                   status.sha256, (int)status.error);
    if (status.state == pulleys::FW_OK) fwOfferActive = false;
}

static void onLegacyOtaStatus(const pulleys::MeshOtaReady& status) {
    if (status.originId != fwTarget) return;
    const char* state = status.state == pulleys::OTA_STATE_OK ? "ok"
                      : status.state == pulleys::OTA_STATE_FAILED ? "failed"
                      : status.state == pulleys::OTA_STATE_DOWNLOADING ? "downloading" : "ready";
    Serial1.printf("FW-STATUS id=%04X env=%s state=%s build_hash=%s sha256=%s err=%d\n",
                   status.originId, status.env, state, fwBuildHash, fwSha,
                   (int)status.err);
    if (status.state == pulleys::OTA_STATE_OK) fwOfferActive = false;
}

static void updateBridgeSelf(uint32_t size, const char* sha) {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!size || strlen(sha) != 64 || !next || size > next->size) {
        Serial1.printf("FW-STATUS id=%04X env=bridge state=failed err=bad_image\n",
                       pulleys::identity_id());
        return;
    }
    strlcpy(fwSha, sha, sizeof(fwSha));
    fwSize = size;
    if (!Update.begin(size, U_FLASH)) {
        Serial1.printf("FW-STATUS id=%04X env=bridge state=failed err=%d\n",
                       pulleys::identity_id(), (int)Update.getError());
        return;
    }
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts_ret(&hash, 0);
    uint8_t buf[384];
    uint32_t off = 0;
    bool ok = true;
    while (off < size) {
        size_t want = size - off;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t got = 0;
        if (!requestFirmwareChunk(off, buf, want, got) || got > want ||
            Update.write(buf, got) != got) { ok = false; break; }
        mbedtls_sha256_update_ret(&hash, buf, got);
        off += got;
        if (off == size || off / 65536 != (off - got) / 65536)
            Serial1.printf("FW-STATUS id=%04X env=bridge state=downloading offset=%lu\n",
                           pulleys::identity_id(), (unsigned long)off);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&hash, digest);
    mbedtls_sha256_free(&hash);
    char actual[65];
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[64] = 0;
    if (!ok || off != size || strcmp(actual, sha) != 0) {
        Update.abort();
        Serial1.printf("FW-STATUS id=%04X env=bridge state=failed err=hash\n",
                       pulleys::identity_id());
        return;
    }
    if (!Update.end(true)) {
        Serial1.printf("FW-STATUS id=%04X env=bridge state=failed err=write\n",
                       pulleys::identity_id());
        return;
    }
    Preferences prefs;
    if (prefs.begin("fw", false)) { prefs.putString("sha256", sha); prefs.end(); }
    Serial1.printf("FW-STATUS id=%04X env=bridge state=ok sha256=%s\n",
                   pulleys::identity_id(), sha);
    Serial.flush(); Serial1.flush();
    delay(500);
    ESP.restart();
}

struct QueuedEvent {
    uint8_t  channel, mode, magnitude, flags;
    uint16_t originId;
};
static QueuedEvent queue[QUEUE_LEN];
static uint8_t     qHead = 0, qTail = 0;

struct QueuedCue { char name[pulleys::MESH_CUE_NAME_MAX + 1]; };
static QueuedCue   cueQueue[QUEUE_LEN];
static uint8_t     cHead = 0, cTail = 0;

// The console is for reading what the bridge is doing, and by default that is
// the cues and nothing else. `v` brings back the per-event lines and the 3 s
// counters, for debugging the radio.
static bool verbose = false;

static uint32_t forwarded = 0;
static uint32_t dropped   = 0;
static uint32_t cuesSent  = 0;

// The mesh callback only queues. The UART write happens in loop(), so a slow or
// unplugged Pi can never stall the radio path.
static void onMeshEvent(const pulleys::MeshEvent& ev, bool relayed) {
    if (!pulleys::mesh_event_is_pull(ev)) return;   // a sensor confirming a remap
    uint8_t next = (qHead + 1) % QUEUE_LEN;
    if (next == qTail) { dropped++; return; }
    queue[qHead] = { ev.channel, ev.mode, ev.magnitude, ev.flags, ev.originId };
    qHead = next;
    if (verbose)
        Serial.printf("★ ch%-2d from 0x%04X %s mag=%3d\n", ev.channel, ev.originId,
                      relayed ? "(relayed)" : "(direct) ", ev.magnitude);
}

static void onMeshCue(const pulleys::MeshCue& c, bool /*relayed*/) {
    uint8_t next = (cHead + 1) % QUEUE_LEN;
    if (next == cTail) { dropped++; return; }
    strlcpy(cueQueue[cHead].name, c.name, sizeof(cueQueue[cHead].name));
    cHead = next;
}

static void drainQueue() {
    while (cTail != cHead) {
        if (Serial1.availableForWrite() < 48) break;
        const char* name = cueQueue[cTail].name;
        Serial1.printf("cue %s\n", name);
        Serial.printf("cue %s\n", name);
        cTail = (cTail + 1) % QUEUE_LEN;
        cuesSent++;
    }

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
    static char buf[256];
    static uint16_t len = 0;
    while (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n' || c == '\r') {
            if (len) {
                buf[len] = 0;
                Serial.printf("  [PI] %s\n", buf);
                if (strncmp(buf, "FW-PING", 7) == 0) {
                    Serial1.printf("FW-BRIDGE id=%04X env=bridge sha256=%s\n",
                                   pulleys::identity_id(), pulleys::whoami_firmware_hash());
                } else if (strncmp(buf, "FW-CENSUS", 9) == 0 ||
                           strncmp(buf, "FW-QUERY", 8) == 0) {
                    int spread = 3000;
                    char* at = strstr(buf, "spread=");
                    if (at) spread = atoi(at + 7);
                    if (spread < 500 || spread > 10000) spread = 3000;
                    pulleys::census_request((uint16_t)spread);
                } else if (strncmp(buf, "FW-OFFER ", 9) == 0) {
                    char id[8] = {}, env[32] = {}, buildHash[65] = {}, sha[65] = {}, md5[33] = {};
                    unsigned size = 0;
                    if (sscanf(buf + 9, "id=%7s env=%31s build_hash=%64s sha256=%64s md5=%32s size=%u",
                               id, env, buildHash, sha, md5, &size) == 6 &&
                        strlen(buildHash) == 64 && strlen(sha) == 64 && strlen(md5) == 32 && size > 0) {
                        fwTarget = (uint16_t)strtoul(id, nullptr, 16);
                        strlcpy(fwEnv, env, sizeof(fwEnv));
                        strlcpy(fwBuildHash, buildHash, sizeof(fwBuildHash));
                        strlcpy(fwSha, sha, sizeof(fwSha));
                        strlcpy(fwMd5, md5, sizeof(fwMd5));
                        fwSize = size;
                        fwOfferActive = true;
                        pulleys::mesh_send_ota_announce("pulleys-fw", "", fwEnv,
                                                        fwTarget, 300, fwMd5);
                        pulleys::mesh_send_fw_offer(fwTarget, fwEnv, fwBuildHash, fwSha, fwSize);
                        Serial1.printf("FW-STATUS id=%04X env=%s state=offered sha256=%s err=0\n",
                                       fwTarget, fwEnv, fwSha);
                    } else {
                        Serial1.println("FW-ERROR reason=bad_offer");
                    }
                } else if (strncmp(buf, "FW-BRIDGE-UPDATE ", 17) == 0) {
                    char sha[65] = {};
                    unsigned size = 0;
                    char* shaAt = strstr(buf, "sha256=");
                    char* sizeAt = strstr(buf, "size=");
                    if (shaAt && sizeAt && sscanf(shaAt, "sha256=%64s", sha) == 1 &&
                        sscanf(sizeAt, "size=%u", &size) == 1)
                        updateBridgeSelf(size, sha);
                    else Serial1.println("FW-STATUS state=failed err=bad_command");
                }
            }
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

// ── Serial console ────────────────────────────────────────────────────────────
//   e<ch>  inject a fake event on that channel, as if the mesh had delivered it
//   v      toggle verbose: per-event lines and the 3 s counters
//   c <name>  inject a fake cue, e.g. "c audio_once_chime"
static void handleSerial() {
    // Console line input; large firmware commands arrive on UART1 instead.
    static char buf[96];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            buf[len] = 0;
            len = 0;
            if (pulleys::whoami_handle(buf)) continue;
            if (pulleys::census_handle_serial(buf)) continue;
            if (buf[0] == 'v' && buf[1] == 0) {
                verbose = !verbose;
                Serial.printf("verbose %s\n", verbose ? "on" : "off");
            } else if (buf[0] == 'c' && buf[1] == ' ') {
                pulleys::MeshCue c = {};
                c.nameLen = (uint8_t)strlcpy(c.name, buf + 2, sizeof(c.name));
                onMeshCue(c, false);
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
    Serial1.setRxBufferSize(1024);
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
    pulleys::census_set_mirror(&Serial1);
    pulleys::mesh_on_fw_status(onFirmwareStatus);
    pulleys::mesh_on_ota_ready(onLegacyOtaStatus);
    pulleys::mesh_on_event(onMeshEvent);
    pulleys::mesh_on_cue(onMeshCue);

    pulleys::census_set_detail("uart1");
    pulleys::whoami_reply();
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("pulleys-fw", "", MESH_WIFI_CHANNEL, 0, 8);
    fwServer.on("/api/v1/health", HTTP_GET, []() {
        fwServer.send(200, "application/json",
                      String("{\"channel\":") + MESH_WIFI_CHANNEL +
                      ",\"busy\":" + (fwOfferActive ? "true" : "false") + "}");
    });
    fwServer.onNotFound([]() {
        if (fwServer.method() == HTTP_GET && fwServer.uri().startsWith("/api/v1/firmware/")) {
            serveFirmware();
            return;
        }
        fwServer.send(404, "text/plain", "not found");
    });
    fwServer.begin();
    Serial.printf("  Firmware AP: pulleys-fw ch=%d ip=%s\n", MESH_WIFI_CHANNEL,
                  WiFi.softAPIP().toString().c_str());
    Serial1.printf("FW-BRIDGE id=%04X env=bridge sha256=%s\n",
                   pulleys::identity_id(), pulleys::whoami_firmware_hash());
    Serial.println("Bridge ready — forwarding mesh events to the Pi.\n");
}

void loop() {
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    pulleys::census_poll();
    fwServer.handleClient();

    // A targeted node offer owns this node while it downloads and installs.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

    handleSerial();
    drainQueue();
    pollPi();

    if (verbose && now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── forwarded=%lu dropped=%lu cues=%lu\n",
                      (unsigned long)forwarded, (unsigned long)dropped,
                      (unsigned long)cuesSent);
        pulleys::mesh_print_stats();
    }
}
