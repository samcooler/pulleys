#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <MD5Builder.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <pulleys_mesh.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>

// ── pulleys_ota — reflashing the piece without touching it ───────────────────
//
// Once the ropes are rigged and the screens are mounted, a USB cable is the
// slow part of every tuning round. This is the way around that, and it is
// deliberately shaped so the installation depends on no WiFi infrastructure
// and the laptop never leaves its own network.
//
// The image travels:  esptool → one host board's spare slot → every node.
//
//   1. tools/ota.py asks the tethered board which flash slot is free, then
//      writes firmware.bin into it with esptool — the same wired path that
//      flashes these boards every day.
//   2. That board is told to HOST: it raises a SoftAP on MESH_WIFI_CHANNEL
//      and serves the image straight out of that slot over HTTP.
//   3. It repeats an ESP-NOW announce naming its AP and the env the image is
//      for. Nodes whose own PULLEYS_ENV matches join the AP and PULL the
//      image; everyone else stays in the art.
//   4. Each node flashes its own inactive slot and reboots into it.
//
// Why this shape:
//
//   The channel is the whole trick. An associated station is pinned to its
//   AP's channel, so raising the SoftAP on MESH_WIFI_CHANNEL means ESP-NOW
//   keeps working for every node throughout — which is what lets the invite,
//   the check-ins and the progress all ride the mesh that already exists.
//
//   Pushing with espota would need the flashing machine on the piece's AP. On
//   a laptop with no ethernet that costs it its internet every single flash,
//   and it leans on the SoftAP forwarding client-to-client traffic, which is
//   not a thing to bet an installation on.
//
//   The image reaches the host through esptool rather than down a serial
//   protocol of our own. Streaming a megabyte into a sketch means writing
//   flow control against a USB CDC stack that silently drops whatever
//   overruns it — which was tried here, and stalled unpredictably about half
//   the time. esptool already solves exactly that problem, reliably, at
//   several hundred KB/s, so the image is simply written where the host can
//   read it and nothing is invented.
//
// The slot used is the host's own inactive OTA partition: it is the one place
// on every board guaranteed to be firmware-sized and not in use. Nothing
// points the bootloader at it — otadata is never touched — so the host keeps
// running what it was running.
//
// A board is never stranded in OTA mode: the window expires and it restarts
// into the art. USB stays the rescue path for anything that goes wrong.
//
// Usage:
//   pulleys::ota_init();                 // after mesh_init(), in setup()
//   pulleys::ota_poll();                 // every loop(), beside mesh_poll()
//   if (pulleys::ota_active()) { ... }   // stop driving the LEDs
//   pulleys::ota_host_begin();           // 'O' on serial — become the host

namespace pulleys {

static const char* OTA_SSID = "pulleys-ota";
static const char* OTA_PASS = "pulleyspulleys";

// Long enough to let a crate of boards pull, short enough that a forgotten
// OTA mode heals itself while you make tea.
static constexpr uint16_t OTA_WINDOW_SECS = 300;
static constexpr uint32_t OTA_ANNOUNCE_MS = 2000;
static constexpr size_t   OTA_IO_CHUNK    = 4096;
static constexpr uint8_t  OTA_PULL_TRIES  = 4;

// The one-line records the host prints. Same rules as PULLEYS-ID: single line,
// space-separated key=value, parse by key and never by position.
#define PULLEYS_OTA_PREFIX "PULLEYS-OTA"

enum : uint8_t {
    OTA_ROLE_IDLE = 0,
    OTA_ROLE_HOST = 1,
    OTA_ROLE_NODE = 2,
};

namespace _ota {

struct NodeRow {
    uint16_t id;
    char     env[16];
    uint8_t  ip[4];
    uint8_t  state;
    int16_t  err;
    uint32_t lastSeen;
    bool     used;
};

struct State {
    uint8_t  role       = OTA_ROLE_IDLE;
    uint32_t windowEnds = 0;

    // Host
    WebServer* server = nullptr;
    const esp_partition_t* part = nullptr;   // where the image sits
    uint32_t imageLen = 0;                   // 0 = nothing to serve yet
    char     imageEnv[16] = {};
    char     imageMd5[33] = {};
    uint16_t idFilter = 0;
    uint32_t lastAnnounce = 0;
    NodeRow  nodes[16] = {};

    // Node
    bool     joined     = false;
    char     apSsid[32] = {};
    char     apPass[16] = {};
    char     wantMd5[33] = {};
};

inline State& S() { static State s; return s; }

inline void ipBytes(const IPAddress& a, uint8_t out[4]) {
    out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; out[3] = a[3];
}

// The host's inactive app slot — firmware-sized by construction, present on
// every board, and not the one we are executing from.
inline const esp_partition_t* imagePartition() {
    State& s = S();
    if (!s.part) s.part = esp_ota_get_next_update_partition(NULL);
    return s.part;
}

// ── Host: the node table ─────────────────────────────────────────────────────
inline void noteNode(const MeshOtaReady& r) {
    State& s = S();
    NodeRow* row = nullptr;
    for (auto& n : s.nodes) if (n.used && n.id == r.originId) { row = &n; break; }
    if (!row) for (auto& n : s.nodes) if (!n.used) { row = &n; break; }
    if (!row) return;   // table full — a bigger install than this was built for

    row->used = true;
    row->id   = r.originId;
    memcpy(row->env, r.env, sizeof(row->env));
    row->env[sizeof(row->env) - 1] = 0;
    memcpy(row->ip, r.ip, 4);
    row->state    = r.state;
    row->err      = r.err;
    row->lastSeen = millis();

    const char* st = r.state == OTA_STATE_READY       ? "ready"
                   : r.state == OTA_STATE_DOWNLOADING ? "downloading"
                   : r.state == OTA_STATE_OK          ? "ok"
                                                      : "failed";
    Serial.printf(PULLEYS_OTA_PREFIX " id=%04X name=N-%04X env=%s ip=%u.%u.%u.%u state=%s err=%d\n",
                  r.originId, r.originId, row->env,
                  row->ip[0], row->ip[1], row->ip[2], row->ip[3], st, (int)r.err);
}

// ── Host: HTTP ───────────────────────────────────────────────────────────────
inline void serveFirmware() {
    State& s = S();
    Serial.printf(PULLEYS_OTA_PREFIX " request uri=%s len=%lu\n",
                  s.server->uri().c_str(), (unsigned long)s.imageLen);
    if (!s.imageLen || !imagePartition()) {
        s.server->send(503, "text/plain", "no image loaded");
        return;
    }
    // A real Content-Length matters: HTTPUpdate sizes its write from it, and
    // setContentLength is the one way to promise it before the body starts.
    s.server->setContentLength(s.imageLen);
    s.server->send(200, "application/octet-stream", "");

    WiFiClient c = s.server->client();
    uint8_t buf[OTA_IO_CHUNK];
    uint32_t sent = 0;
    while (sent < s.imageLen && c.connected()) {
        size_t chunk = s.imageLen - sent;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(s.part, sent, buf, chunk) != ESP_OK) break;
        size_t w = c.write(buf, chunk);
        if (w == 0) break;
        sent += w;
    }
    Serial.printf(PULLEYS_OTA_PREFIX " served %lu/%lu bytes\n",
                  (unsigned long)sent, (unsigned long)s.imageLen);
}

inline void serveNodes() {
    State& s = S();
    String j = "[";
    bool first = true;
    for (auto& n : s.nodes) {
        if (!n.used) continue;
        if (!first) j += ",";
        first = false;
        char idbuf[8];
        snprintf(idbuf, sizeof(idbuf), "%04X", n.id);
        j += "{\"id\":\"";
        j += idbuf;
        j += "\",\"env\":\"";
        j += n.env;
        j += "\",\"ip\":\"";
        j += String(n.ip[0]) + "." + String(n.ip[1]) + "." + String(n.ip[2]) + "." + String(n.ip[3]);
        j += "\",\"state\":";
        j += String(n.state);
        j += "}";
    }
    j += "]";
    s.server->send(200, "application/json", j);
}

// Hash what esptool actually left in the slot. The write is reliable, but an
// image served from the wrong offset or a half-finished write would fail on
// every node at once and look like a radio problem, which is a miserable
// thing to debug from the far end.
inline bool verifyImage(uint32_t len, const char* wantMd5) {
    const esp_partition_t* p = imagePartition();
    if (!p) return false;
    if (len > p->size) {
        Serial.printf(PULLEYS_OTA_PREFIX " image %lu does not fit slot %lu\n",
                      (unsigned long)len, (unsigned long)p->size);
        return false;
    }
    MD5Builder md5;
    md5.begin();
    uint8_t buf[OTA_IO_CHUNK];
    uint32_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_partition_read(p, off, buf, chunk) != ESP_OK) {
            Serial.println(PULLEYS_OTA_PREFIX " flash read failed");
            return false;
        }
        md5.add(buf, (uint16_t)chunk);
        off += chunk;
    }
    md5.calculate();
    String got = md5.toString();
    if (wantMd5 && wantMd5[0] && !got.equalsIgnoreCase(wantMd5)) {
        Serial.printf(PULLEYS_OTA_PREFIX " image BAD md5 want=%s got=%s\n", wantMd5, got.c_str());
        return false;
    }
    Serial.printf(PULLEYS_OTA_PREFIX " image ok size=%lu md5=%s\n",
                  (unsigned long)len, got.c_str());
    return true;
}

}  // namespace _ota

// ── What this board last took ────────────────────────────────────────────────
// Kept in NVS because the fact has to survive the reboot it causes: the node
// flashes, restarts into the new image, and comes back to an announce that is
// still being repeated. Without a memory of what it already applied it takes
// the same image again, and again, for the length of the window.
inline void ota_remember_applied(const char* md5) {
    if (!md5 || !md5[0]) return;
    Preferences p;
    if (!p.begin("ota", false)) return;
    p.putString("md5", md5);
    p.end();
}

inline bool ota_already_applied(const char* md5) {
    if (!md5 || !md5[0]) return false;   // an announce with no md5 is always new
    Preferences p;
    if (!p.begin("ota", true)) return false;
    String have = p.getString("md5", "");
    p.end();
    return have.equalsIgnoreCase(md5);
}

// ── Public API ───────────────────────────────────────────────────────────────

inline bool ota_active() { return _ota::S().role != OTA_ROLE_IDLE; }
inline uint8_t ota_role() { return _ota::S().role; }

// Become the host. Raises the SoftAP on the mesh channel and starts serving.
inline bool ota_host_begin(uint16_t windowSecs = OTA_WINDOW_SECS) {
    _ota::State& s = _ota::S();
    if (s.role == OTA_ROLE_HOST) {
        s.windowEnds = millis() + (uint32_t)windowSecs * 1000;
        Serial.println(PULLEYS_OTA_PREFIX " host window extended");
        return true;
    }

    // AP_STA, not AP: the mesh has to keep running underneath this.
    WiFi.mode(WIFI_AP_STA);
    bool up = WiFi.softAP(OTA_SSID, OTA_PASS, MESH_WIFI_CHANNEL, 0, 10);
    esp_wifi_set_ps(WIFI_PS_NONE);

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);

    if (!up) {
        Serial.println(PULLEYS_OTA_PREFIX " softAP FAILED");
        return false;
    }
    if (ch != MESH_WIFI_CHANNEL) {
        // Worth shouting about: the AP landing elsewhere takes the whole mesh
        // with it, and every node goes quiet at once.
        Serial.printf(PULLEYS_OTA_PREFIX " !! AP on ch %d, mesh wants %d\n",
                      ch, MESH_WIFI_CHANNEL);
    }

    if (!s.server) {
        s.server = new WebServer(80);
        s.server->on("/firmware.bin", _ota::serveFirmware);
        s.server->on("/nodes", _ota::serveNodes);
        // A node that asks for the wrong thing reports only "file not found",
        // which says nothing about what it asked for. Say it here instead.
        s.server->onNotFound([]() {
            WebServer* sv = _ota::S().server;
            Serial.printf(PULLEYS_OTA_PREFIX " 404 method=%d uri=%s\n",
                          (int)sv->method(), sv->uri().c_str());
            sv->send(404, "text/plain", "not found");
        });
        s.server->begin();
    }

    s.role       = OTA_ROLE_HOST;
    s.windowEnds = millis() + (uint32_t)windowSecs * 1000;
    mesh_on_ota_ready(_ota::noteNode);

    Serial.printf(PULLEYS_OTA_PREFIX " host up ssid=%s ch=%d ip=%s window=%us\n",
                  OTA_SSID, ch, WiFi.softAPIP().toString().c_str(), windowSecs);
    return true;
}

// Called on the node when the host's announce names this board's env.
inline void ota_node_join(const MeshOtaAnnounce& a) {
    _ota::State& s = _ota::S();
    if (s.role != OTA_ROLE_IDLE) return;

    s.role       = OTA_ROLE_NODE;
    s.windowEnds = millis() + (uint32_t)a.windowSecs * 1000;
    strncpy(s.apSsid, a.ssid, sizeof(s.apSsid) - 1);
    strncpy(s.apPass, a.pass, sizeof(s.apPass) - 1);
    strncpy(s.wantMd5, a.md5, sizeof(s.wantMd5) - 1);

    Serial.printf(PULLEYS_OTA_PREFIX " invited for env=%s — joining %s\n", a.env, s.apSsid);
    WiFi.begin(s.apSsid, s.apPass);
}

inline void ota_poll() {
    _ota::State& s = _ota::S();
    if (s.role == OTA_ROLE_IDLE) return;

    // Nothing keeps a board in OTA mode past its window. Restarting is the
    // one recovery that works from every state, including a half-joined one.
    if ((int32_t)(millis() - s.windowEnds) > 0) {
        Serial.println(PULLEYS_OTA_PREFIX " window over — restarting into the art");
        delay(50);
        ESP.restart();
    }

    if (s.role == OTA_ROLE_HOST) {
        s.server->handleClient();
        if (s.imageLen && millis() - s.lastAnnounce >= OTA_ANNOUNCE_MS) {
            s.lastAnnounce = millis();
            uint16_t left = (uint16_t)((s.windowEnds - millis()) / 1000);
            mesh_send_ota_announce(OTA_SSID, OTA_PASS, s.imageEnv, s.idFilter, left,
                                   s.imageMd5);
        }
        return;
    }

    // Node
    if (!s.joined) {
        if (WiFi.status() != WL_CONNECTED) return;
        s.joined = true;
        uint8_t ip[4];
        _ota::ipBytes(WiFi.localIP(), ip);
        Serial.printf(PULLEYS_OTA_PREFIX " joined ip=%s — pulling image\n",
                      WiFi.localIP().toString().c_str());
        mesh_send_ota_ready(PULLEYS_ENV, ip, OTA_STATE_READY);
        delay(30);
        mesh_send_ota_ready(PULLEYS_ENV, ip, OTA_STATE_DOWNLOADING);

        // The pull is done by hand rather than through HTTPUpdate, so the
        // HTTP status and the byte count are visible when something goes
        // wrong. HTTPUpdate collapses every failure into one code and reports
        // "file not found" for anything that is not a 200, which is not enough
        // to debug from the far end of a rope.
        int16_t err = 0;
        bool done = false;

        // Retried in place rather than by rebooting. The first GET after
        // associating is answered with a 404 often enough to matter -- the
        // host's own log shows no request arriving for those, so something
        // between the two radios is eating it -- and a second attempt a moment
        // later almost always succeeds. Rebooting to retry works too, but it
        // costs a boot and a rejoin each time and makes the log unreadable.
        for (uint8_t attempt = 1; attempt <= OTA_PULL_TRIES && !done; attempt++) {
        WiFiClient client;
        HTTPClient http;
        http.setReuse(false);
        http.setTimeout(20000);

        int code = -1, len = 0;
        err = 0;

        if (!http.begin(client, "http://192.168.4.1/firmware.bin")) {
            err = -1;
            Serial.println(PULLEYS_OTA_PREFIX " could not open the connection");
        } else {
            code = http.GET();
            len  = http.getSize();
            Serial.printf(PULLEYS_OTA_PREFIX " GET -> %d, %d bytes\n", code, len);
            if (code != HTTP_CODE_OK) {
                err = (int16_t)code;
                String body = http.getString();
                if (body.length() > 100) body = body.substring(0, 100);
                Serial.printf(PULLEYS_OTA_PREFIX " refused: [%s]\n", body.c_str());
            } else if (len <= 0) {
                err = -101;                       // no length, nothing to size the write from
                Serial.println(PULLEYS_OTA_PREFIX " server did not report a size");
            } else if (!Update.begin((size_t)len)) {
                err = (int16_t)Update.getError();
                Serial.printf(PULLEYS_OTA_PREFIX " Update.begin failed: %s\n",
                              Update.errorString());
            } else {
                size_t written = Update.writeStream(http.getStream());
                if (written != (size_t)len) {
                    err = (int16_t)Update.getError();
                    Serial.printf(PULLEYS_OTA_PREFIX " wrote %u of %d bytes: %s\n",
                                  (unsigned)written, len, Update.errorString());
                    Update.abort();
                } else if (!Update.end(true)) {
                    err = (int16_t)Update.getError();
                    Serial.printf(PULLEYS_OTA_PREFIX " Update.end failed: %s\n",
                                  Update.errorString());
                } else {
                    done = true;
                }
            }
            http.end();
        }

        if (!done && attempt < OTA_PULL_TRIES) {
            Serial.printf(PULLEYS_OTA_PREFIX " attempt %u/%u failed (err=%d) — retrying\n",
                          attempt, OTA_PULL_TRIES, (int)err);
            delay(1200);
        }
        }  // attempts

        if (done) {
            // Remember it across the reboot, or this board will come back up,
            // hear the same announce still repeating, and take the same image
            // again for as long as the window lasts.
            ota_remember_applied(s.wantMd5);
            mesh_send_ota_ready(PULLEYS_ENV, ip, OTA_STATE_OK);
            Serial.println(PULLEYS_OTA_PREFIX " written — rebooting into the new image");
            delay(150);
            ESP.restart();
        } else {
            // Report the reason over the air before restarting: this board may
            // be twelve feet up a rope, where its serial port is of no use to
            // anyone.
            mesh_send_ota_ready(PULLEYS_ENV, ip, OTA_STATE_FAILED, err);
            Serial.printf(PULLEYS_OTA_PREFIX " FAILED err=%d — restarting\n", (int)err);
            delay(150);
            ESP.restart();
        }
    }
}

// Wire the announce up. Every role calls this in setup(), after mesh_init().
// A board acts on an announce only when the image is for its own env and, if
// the host named one, its own device ID — so flashing the sensors never
// disturbs the screens.
inline void ota_init() {
    mesh_on_ota_announce([](const MeshOtaAnnounce& a) {
        if (_ota::S().role != OTA_ROLE_IDLE) return;          // host, or already going
        if (strncmp(a.env, PULLEYS_ENV, sizeof(a.env)) != 0) return;
        if (a.idFilter != 0 && a.idFilter != identity_id()) return;
        if (ota_already_applied(a.md5)) return;   // this is what I am running
        ota_node_join(a);
    });
}

// Serial commands for the host side. Returns true if the line was ours.
//
//   OTA-SLOT                                  → where to write the image
//   OTA-SERVE env=<env> size=<n> md5=<hex> id=<hex|0>
//                                             → verify that slot and announce
//
// The tool asks for the slot, writes the image there with esptool, then asks
// the board to serve it. Nothing large ever crosses this port.
inline bool ota_handle_serial(const char* line) {
    if (strncmp(line, "OTA-SLOT", 8) == 0) {
        const esp_partition_t* p = _ota::imagePartition();
        if (!p) {
            Serial.println(PULLEYS_OTA_PREFIX " slot none — board has no spare OTA partition");
            return true;
        }
        Serial.printf(PULLEYS_OTA_PREFIX " slot label=%s offset=0x%06X size=0x%06X\n",
                      p->label, (unsigned)p->address, (unsigned)p->size);
        return true;
    }

    if (strncmp(line, "OTA-STOP", 8) == 0) {
        // The tool says everyone is done, so stop inviting. Restarting is how
        // the host gets back into the art, and it is the same path the window
        // expiry takes -- one way back, not two.
        Serial.println(PULLEYS_OTA_PREFIX " stopping — restarting into the art");
        Serial.flush();
        delay(50);
        ESP.restart();
        return true;
    }

    if (strncmp(line, "OTA-SERVE", 9) == 0) {
        char env[16] = {};
        char md5[33] = {};
        unsigned long size = 0;
        unsigned idf = 0;
        const char* p;
        if ((p = strstr(line, "env=")))  sscanf(p + 4, "%15s", env);
        if ((p = strstr(line, "size="))) sscanf(p + 5, "%lu", &size);
        if ((p = strstr(line, "md5=")))  sscanf(p + 4, "%32s", md5);
        if ((p = strstr(line, "id=")))   sscanf(p + 3, "%x", &idf);

        if (!_ota::verifyImage((uint32_t)size, md5)) return true;

        _ota::State& s = _ota::S();
        s.imageLen = (uint32_t)size;
        s.idFilter = (uint16_t)idf;
        strncpy(s.imageEnv, env, sizeof(s.imageEnv) - 1);
        strncpy(s.imageMd5, md5, sizeof(s.imageMd5) - 1);
        if (s.role != OTA_ROLE_HOST) ota_host_begin();
        Serial.printf(PULLEYS_OTA_PREFIX " serving env=%s id=%04X — announcing\n",
                      s.imageEnv, s.idFilter);
        return true;
    }
    return false;
}

}  // namespace pulleys
