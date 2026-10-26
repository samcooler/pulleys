#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <pulleys_mesh.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>

// Node-side pull OTA. The bridge is the always-on AP and HTTP endpoint; it
// fetches bounded image chunks from the Overlord over UART as nodes request it.
namespace pulleys {

enum : uint8_t { OTA_ROLE_IDLE = 0, OTA_ROLE_NODE = 2 };
static constexpr uint8_t FW_READY = 0, FW_DOWNLOADING = 1, FW_OK = 2, FW_FAILED = 3;

namespace _ota {
struct State {
    bool pending = false;
    bool active = false;
    uint16_t targetId = 0;
    char env[32] = {};
    char buildHash[65] = {};
    char sha256[65] = {};
    uint32_t size = 0;
    uint32_t lastOfferAt = 0;
};
inline State& S() { static State s; return s; }

inline void onOffer(const MeshFwOffer& offer) {
    State& s = S();
    if (s.pending || s.active) return;
    if (offer.targetId != identity_id() || strncmp(offer.env, PULLEYS_ENV, sizeof(offer.env)) != 0) return;
    // Repeated mesh copies are ignored while pending/active above. Keep only a
    // short post-failure throttle so the coordinator's next retry is accepted.
    if (s.lastOfferAt && millis() - s.lastOfferAt < 1000) return;
    if (strlen(offer.buildHash) != 64 || strlen(offer.sha256) != 64 || !offer.size) return;
    if (strcmp(whoami_firmware_hash(), offer.buildHash) == 0) return;
    s.targetId = offer.targetId;
    strlcpy(s.env, offer.env, sizeof(s.env));
    strlcpy(s.buildHash, offer.buildHash, sizeof(s.buildHash));
    strlcpy(s.sha256, offer.sha256, sizeof(s.sha256));
    s.size = offer.size;
    s.lastOfferAt = millis();
    s.pending = true;
    s.active = false;
    mesh_send_fw_status(FW_READY, s.env, s.buildHash, s.sha256);
    Serial.printf("[FW] offer accepted env=%s sha256=%s\n", s.env, s.sha256);
}

inline bool pullAndInstall(State& s) {
    WiFi.mode(WIFI_STA);
    WiFi.begin("pulleys-fw"); // open bridge AP, fixed to the mesh channel
    uint32_t deadline = millis() + 20000;
    while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
        mesh_poll();
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -1);
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://192.168.4.1/api/v1/firmware/%s", s.sha256);
    HTTPClient http;
    http.setTimeout(30000);
    if (!http.begin(url)) {
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -2);
        return false;
    }
    int code = http.GET();
    int length = http.getSize();
    if (code != HTTP_CODE_OK || length <= 0) {
        http.end();
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -3);
        return false;
    }
    if ((uint32_t)length != s.size) {
        http.end();
        Update.abort();
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -6);
        return false;
    }
    if (!Update.begin((size_t)length, U_FLASH)) {
        http.end();
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -3);
        return false;
    }

    mesh_send_fw_status(FW_DOWNLOADING, s.env, s.buildHash, s.sha256);
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts_ret(&hash, 0);
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    size_t total = 0;
    bool ok = true;
    while (total < (size_t)length) {
        size_t want = (size_t)length - total;
        if (want > sizeof(buffer)) want = sizeof(buffer);
        size_t got = stream->readBytes(buffer, want);
        if (!got) { ok = false; break; }
        mbedtls_sha256_update_ret(&hash, buffer, got);
        if (Update.write(buffer, got) != got) { ok = false; break; }
        total += got;
        mesh_poll();
        yield();
    }
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&hash, digest);
    mbedtls_sha256_free(&hash);
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = 0;
    http.end();

    if (!ok || total != (size_t)length || strcmp(hex, s.sha256) != 0) {
        Update.abort();
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -4);
        Serial.println("[FW] image length or SHA-256 mismatch");
        return false;
    }
    if (!Update.end(true)) {
        mesh_send_fw_status(FW_FAILED, s.env, s.buildHash, s.sha256, -5);
        return false;
    }
    Preferences prefs;
    if (prefs.begin("fw", false)) { prefs.putString("sha256", s.sha256); prefs.end(); }
    mesh_send_fw_status(FW_OK, s.env, s.buildHash, s.sha256);
    Serial.println("[FW] image verified and installed; rebooting");
    delay(500);
    ESP.restart();
    return true;
}
} // namespace _ota

inline void ota_init() { mesh_on_fw_offer(_ota::onOffer); }
inline bool ota_active() { return _ota::S().pending || _ota::S().active; }
inline void ota_poll() {
    _ota::State& s = _ota::S();
    if (!s.pending || s.active) return;
    s.pending = false;
    s.active = true;
    _ota::pullAndInstall(s);
    s.active = false;
}

} // namespace pulleys
