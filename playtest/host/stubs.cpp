// Stand-ins for what gamestate.cpp and components.cpp call outside themselves.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <pulleys_channel.h>
#include <random>
#include "devices.h"
#include "gamestate.h"
#include "installcfg.h"

uint32_t g_millis = 0;
static std::mt19937 s_rng(1);
long random(long max) { return max <= 0 ? 0 : (long)(s_rng() % (unsigned long)max); }
long random(long min, long max) { return min + random(max - min); }

SerialShim Serial;
static bool verbose() { return getenv("ENGINE_VERBOSE") != nullptr; }
int SerialShim::printf(const char* fmt, ...) {
    if (!verbose()) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}
void SerialShim::println(const char* s) { if (verbose()) fprintf(stderr, "%s\n", s); }
void SerialShim::print(const char* s) { if (verbose()) fputs(s, stderr); }

namespace pulleys {
static uint8_t s_palette[CHANNEL_COUNT];
uint8_t* palette_table() { return s_palette; }

bool mesh_send_cue(const char* name, uint8_t flags) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > MESH_CUE_NAME_MAX) return false;
    printf("cue %s %d\n", name, (flags & MESH_CUE_FLAG_ENTER) ? 1 : 0);
    return true;
}
bool mesh_send_flood(uint8_t msgType, const void* body, uint8_t len, uint8_t, uint16_t) {
    if (msgType == MESH_MSG_GAME_INFO) printf("info %.*s\n", (int)len, (const char*)body);
    return true;
}
void mesh_send_components(const uint8_t*, const uint8_t*, uint16_t, bool) {}
}  // namespace pulleys

void stubs_reset(uint32_t seed) {
    s_rng.seed(seed);
    g_millis = 0;
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT; ch++)
        pulleys::s_palette[ch] = (uint8_t)((ch + pulleys::COLOR_COUNT - 1) % pulleys::COLOR_COUNT);
}

// ── devices.cpp: settings print, inputs checked against the game ────────────
bool devices_validate(JsonDocument&, String&) { return true; }
bool devices_check_set(JsonDocument&, JsonVariant, const String&, String&) { return true; }
bool devices_is_input(JsonDocument& doc, const char* name) {
    const char* dot = strchr(name, '.');
    if (!dot) return false;
    std::string dev(name, dot - name);
    for (JsonVariant e : doc["devices"][dev.c_str()]["emits"].as<JsonArray>())
        if (strcmp(e.as<const char*>(), dot + 1) == 0) return true;
    return false;
}
void devices_load(JsonDocument&) {}
void devices_set(const char* key, const char* value) { printf("set %s=%s\n", key, value); }

// ── installcfg.cpp: ropes on channels 1-20, the game's shuffle ───────────────
static pulleys::MeshConfigRow s_rows[20];
static bool    s_shuffled = false;
static uint8_t s_day[pulleys::CHANNEL_COUNT];
uint8_t installcfg_row_count() { return 20; }
const pulleys::MeshConfigRow* installcfg_rows() {
    for (uint8_t i = 0; i < 20; i++) s_rows[i] = {(uint16_t)(0x100 + i), (uint8_t)(i + 1)};
    return s_rows;
}
void installcfg_game_shuffle() {
    if (!s_shuffled) { memcpy(s_day, pulleys::s_palette, sizeof(s_day)); s_shuffled = true; }
    uint8_t perm[5];
    bool moved = false;
    while (!moved) {
        for (uint8_t i = 0; i < 5; i++) perm[i] = i;
        for (uint8_t i = 4; i > 0; i--) { uint8_t j = (uint8_t)random(i + 1); uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (uint8_t i = 0; i < 5; i++) if (perm[i] != i) moved = true;
    }
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT; ch++)
        if (pulleys::s_palette[ch] < 5) pulleys::s_palette[ch] = perm[pulleys::s_palette[ch]];
    gamestate_note_palette();
}
void installcfg_game_restore() {
    if (!s_shuffled) return;
    memcpy(pulleys::s_palette, s_day, sizeof(s_day));
    s_shuffled = false;
    gamestate_note_palette();
}
