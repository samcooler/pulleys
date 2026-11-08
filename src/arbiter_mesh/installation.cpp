#include "installation.h"
#include "installcfg.h"
#include "installmap.h"
#include "clocksrc.h"
#include "gamestate.h"
#include "devices.h"
#include "monitor.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <pulleys_channel.h>
#include <pulleys_identity.h>
#include <pulleys_mesh.h>

namespace {
constexpr size_t CHUNK = 200;
constexpr size_t MAX_RESPONSE = CHUNK * 64;
constexpr uint8_t CACHE_SIZE = 8;
struct Reply {
    String request;
    String response;
    uint16_t bridge = 0;
    String q;
};
Reply cache[CACHE_SIZE];
uint8_t nextSlot = 0;
int sending = -1;
size_t offset = 0;
uint32_t lastSend = 0;
char boot[17];

bool hex(const char* s, size_t size) {
    if (!s || strlen(s) != size) return false;
    for (size_t i = 0; i < size; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

void sensorSnapshot(JsonObject row, uint16_t id) {
    char sensor[5]; snprintf(sensor, sizeof(sensor), "%04X", id);
    row["id"] = sensor;
    int assigned = installcfg_channel_of(id);
    if (assigned >= 0) row["assigned"] = assigned;
    else row["assigned"] = nullptr;
    row["reported"] = nullptr;
    row["seen_ms_ago"] = nullptr;
    const MonNode* nodes = monitor_nodes();
    for (uint8_t n = 0; n < MON_MAX_NODES; ++n) {
        if (!nodes[n].active || nodes[n].type != pulleys::MESH_ORIGIN_SENSOR || nodes[n].id != id) continue;
        uint32_t seen = max(nodes[n].lastBeaconMs, nodes[n].lastEventMs);
        row["seen_ms_ago"] = (uint32_t)(millis() - seen);
        if (nodes[n].known) row["reported"] = nodes[n].channel;
        break;
    }
}

void snapshot(JsonObject out) {
    char id[5];
    snprintf(id, sizeof(id), "%04X", pulleys::identity_id());
    out["id"] = id;
    out["boot"] = boot;
    out["revision"] = installcfg_revision();
    out["storage"] = installcfg_storage();
    out["game_shuffled"] = installcfg_game_shuffled();
    out["uptime_ms"] = millis();
    JsonObject game = out["game"].to<JsonObject>();
    game["ready"] = gamestate_ready();
    game["name"] = gamestate_game();
    game["state"] = gamestate_state();
    game["state_age_ms"] = gamestate_state_age_ms();
    game["timer_left_ms"] = gamestate_timer_left_ms();
    game["timer_target"] = gamestate_timer_target();
    game["goals_visible"] = gamestate_goals_visible();
    JsonArray goals = game["goal_colors"].to<JsonArray>();
    uint8_t goalMask = gamestate_goal_color_mask();
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; ++c)
        if (goalMask & (1u << c)) goals.add(c);
    char deviceStatus[80];
    game["devices_ok"] = devices_summary(deviceStatus, sizeof(deviceStatus));
    game["devices_status"] = deviceStatus;
    JsonObject mesh = out["mesh"].to<JsonObject>();
    mesh["sensors"] = monitor_count(pulleys::MESH_ORIGIN_SENSOR);
    mesh["screens"] = monitor_count(pulleys::MESH_ORIGIN_SCREEN);
    mesh["events"] = monitor_total_events();
    mesh["events_per_min"] = monitor_events_per_min();
    mesh["skew_spread_ms"] = monitor_skew_spread();
    JsonArray channels = out["channels"].to<JsonArray>();
    const uint8_t* daily = installcfg_daily_colors();
    for (uint8_t ch = 1; ch <= 24; ++ch) {
        JsonObject row = channels.add<JsonObject>();
        row["channel"] = ch;
        row["color"] = pulleys::channel_color_index(ch);
        row["daily_color"] = daily[ch];
        uint8_t ropes = 0;
        for (uint8_t i = 0; i < installcfg_row_count(); ++i)
            if (installcfg_rows()[i].channel == ch) ++ropes;
        row["ropes"] = ropes;
    }
    JsonArray colors = out["colors"].to<JsonArray>();
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; ++c) {
        JsonObject row = colors.add<JsonObject>();
        row["name"] = pulleys::color_name(c);
        CRGB value = pulleys::color_rgb(c);
        JsonArray rgb = row["rgb"].to<JsonArray>();
        rgb.add(value.r); rgb.add(value.g); rgb.add(value.b);
    }
    JsonArray sensors = out["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < installcfg_row_count(); ++i) {
        sensorSnapshot(sensors.add<JsonObject>(), installcfg_rows()[i].id);
    }
    const auto& session = installmap_state();
    JsonObject mapping = out["mapping"].to<JsonObject>();
    mapping["active"] = session.active;
    mapping["kind"] = session.owner ? "remote" : "touch";
    char owner[17]; snprintf(owner, sizeof(owner), "%08lX%08lX", (unsigned long)(session.owner >> 32), (unsigned long)session.owner);
    mapping["owner"] = owner;
    mapping["phase"] = session.phase == MappingSession::WAIT ? "wait" : session.phase == MappingSession::LISTEN ? "listen" : "pick";
    mapping["assigned"] = session.assigned;
    uint32_t elapsed = millis() - session.listenAt;
    mapping["listen_left_ms"] = session.phase == MappingSession::LISTEN && elapsed < MappingSession::LISTEN_MS ? MappingSession::LISTEN_MS - elapsed : 0;
    mapping["lease_left_ms"] = session.active && session.owner ? MappingSession::LEASE_MS - min(MappingSession::LEASE_MS, (uint32_t)(millis() - session.renewedAt)) : 0;
    JsonArray captured = mapping["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < session.count; ++i) sensorSnapshot(captured.add<JsonObject>(), session.ids[i]);
    JsonObject clock = out["clock"].to<JsonObject>();
    clock["present"] = clocksrc_present();
    clock["source"] = clocksrc_source_str();
    clock["storage"] = clocksrc_storage();
    pulleys::RtcTime time;
    if (clocksrc_read(time)) {
        char formatted[24]; pulleys::RTC::format(time, formatted, sizeof(formatted));
        clock["time"] = formatted;
    } else clock["time"] = nullptr;
}

bool integer(JsonVariantConst v, int lo, int hi) {
    return v.is<int>() && v.as<int>() >= lo && v.as<int>() <= hi;
}

const char* apply(JsonObjectConst in, uint16_t bridge) {
    const char* op = in["op"] | "";
    if (!strcmp(op, "get")) return nullptr;
    if (!in["boot"].is<const char*>() || strcmp(in["boot"], boot)) return "conflict";
    bool sessionCleanup = !strcmp(op, "map_renew") || !strcmp(op, "map_end");
    if (!sessionCleanup && (!in["revision"].is<uint32_t>() || in["revision"].as<uint32_t>() != installcfg_revision())) return "conflict";
    if (!strncmp(op, "map_", 4)) {
        const char* owner = in["session"] | "";
        if (!hex(owner, 16)) return "invalid";
        uint64_t token = strtoull(owner, nullptr, 16);
        if (!token) return "invalid";
        auto& session = installmap_state();
        if (!strcmp(op, "map_begin")) return installmap_open(token, bridge) ? nullptr : "mapping_busy";
        if (!session.owns(token, bridge)) return "mapping_session_lost";
        if (!strcmp(op, "map_renew")) return session.renew(token, bridge, millis()) ? nullptr : "mapping_session_lost";
        if (!strcmp(op, "map_end")) return installmap_close(token, bridge) ? nullptr : "mapping_session_lost";
        if (!strcmp(op, "map_again")) { installmap_reset(); return nullptr; }
        if (!strcmp(op, "map_assign")) {
            if (!integer(in["channel"], 1, 24)) return "invalid";
            return installmap_assign(in["channel"]) ? nullptr : "capture_not_ready_or_map_full";
        }
        return "unsupported";
    }
    if (!strcmp(op, "clock")) {
        const char* text = in["time"] | "";
        if (strlen(text) != 14) return "invalid";
        for (uint8_t i = 0; i < 14; ++i) if (text[i] < '0' || text[i] > '9') return "invalid";
        auto number = [&](uint8_t start, uint8_t length) {
            int value = 0;
            for (uint8_t i = 0; i < length; ++i) value = value * 10 + text[start + i] - '0';
            return value;
        };
        pulleys::RtcTime time = {};
        time.year = number(0, 4); time.month = number(4, 2); time.day = number(6, 2);
        time.hour = number(8, 2); time.minute = number(10, 2); time.second = number(12, 2);
        return clocksrc_set(time) ? nullptr : "clock_set_failed";
    }
    if (!strcmp(op, "color")) {
        if (!integer(in["channel"], 1, 24) || !integer(in["color"], 0, 5)) return "invalid";
        // The setters return 'changed', so assigning an already-current value
        // is still a successful, idempotent operation.
        installcfg_set_color(in["channel"], in["color"]);
    } else if (!strcmp(op, "rgb")) {
        if (!integer(in["color"], 0, 5) || !integer(in["r"], 0, 255) ||
            !integer(in["g"], 0, 255) || !integer(in["b"], 0, 255)) return "invalid";
        installcfg_set_rgb(in["color"], in["r"], in["g"], in["b"]);
    } else if (!strcmp(op, "shuffle")) {
        installcfg_shuffle_game1();
    } else if (!strcmp(op, "reset_rgb")) {
        installcfg_reset_rgb();
    } else return "unsupported";
    return nullptr;
}

void onRequest(const pulleys::MeshFlood& f, bool) {
    if (f.h.originType != pulleys::MESH_ORIGIN_BRIDGE) return;
    JsonDocument doc;
    if (deserializeJson(doc, f.body, f.len) || !doc.is<JsonObject>()) return;
    JsonObjectConst in = doc.as<JsonObjectConst>();
    const char* q = in["q"] | "";
    const char* target = in["target"] | "";
    const char* op = in["op"] | "";
    if (!in["v"].is<int>() || in["v"].as<int>() != 1 || !hex(q, 16) || !hex(target, 4)) return;
    uint16_t destination = strtoul(target, nullptr, 16);
    if (destination != pulleys::identity_id() && (destination != 0 || strcmp(op, "get"))) return;
    String request;
    serializeJson(doc, request);
    for (uint8_t i = 0; i < CACHE_SIZE; ++i) {
        if (cache[i].bridge != f.h.originId || cache[i].q != q) continue;
        // Reusing an ID with another body must never execute a second action.
        if (cache[i].request != request) return;
        if (sending < 0) { sending = i; offset = 0; }
        return;
    }
    // One response at a time. The caller retries the same request if busy.
    if (sending >= 0) return;
    installmap_tick();
    const char* error = apply(in, f.h.originId);
    JsonDocument response;
    response["v"] = 1;
    response["ok"] = error == nullptr;
    if (error) response["error"] = error;
    snapshot(response["installation"].to<JsonObject>());
    Reply& reply = cache[nextSlot];
    reply.request = request;
    reply.bridge = f.h.originId;
    reply.q = q;
    reply.response = "";
    serializeJson(response, reply.response);
    if (reply.response.length() > MAX_RESPONSE) {
        reply.response = "{\"v\":1,\"ok\":false,\"error\":\"snapshot_too_large\"}";
    }
    sending = nextSlot;
    nextSlot = (nextSlot + 1) % CACHE_SIZE;
    offset = 0;
}
} // namespace

void installation_init() {
    snprintf(boot, sizeof(boot), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
    pulleys::mesh_on_flood(pulleys::MESH_MSG_INSTALL_REQ, onRequest);
}

void installation_tick() {
    if (sending < 0 || millis() - lastSend < 100) return;
    lastSend = millis();
    Reply& reply = cache[sending];
    char packet[pulleys::MESH_FLOOD_MAX_BODY + 1];
    unsigned part = offset / CHUNK;
    unsigned count = (reply.response.length() + CHUNK - 1) / CHUNK;
    int header = snprintf(packet, sizeof(packet), "%s %04X %02u %02u ", reply.q.c_str(), reply.bridge, part, count);
    size_t length = min(CHUNK, reply.response.length() - offset);
    memcpy(packet + header, reply.response.c_str() + offset, length);
    // Each chunk is independently deduped by the mesh. A whole-response retry
    // uses new flood sequences but the same cached application request ID.
    pulleys::mesh_send_flood(pulleys::MESH_MSG_INSTALL_RES, packet, header + length, 1);
    offset += length;
    if (offset >= reply.response.length()) sending = -1;
}
