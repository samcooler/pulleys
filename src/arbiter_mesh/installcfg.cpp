#include "installcfg.h"
#include <Arduino.h>
#include <Preferences.h>
#include <pulleys_channel.h>
#include <pulleys_install.h>
#include <stdlib.h>
#include <string.h>

#define NVS_NS         "installcfg"
#define REPEAT_MS      5000    // how often the whole config is re-sent unchanged

static pulleys::MeshConfigRow s_rows[pulleys::MESH_CONFIG_MAX_ROWS];
static uint8_t  s_count    = 0;
static bool     s_dirty    = false;   // changed since the last send: save + burst
static uint32_t s_lastSend = 0;

static void saveMap() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBytes("map", s_rows, (size_t)s_count * sizeof(pulleys::MeshConfigRow));
    p.putBool("seeded", true);
    p.end();
}

static int findRow(uint16_t id) {
    for (uint8_t i = 0; i < s_count; i++) if (s_rows[i].id == id) return i;
    return -1;
}

void installcfg_init() {
    Preferences p;
    p.begin(NVS_NS, true);
    bool seeded = p.getBool("seeded", false);
    size_t n = seeded ? p.getBytes("map", s_rows, sizeof(s_rows)) : 0;
    p.end();
    s_count = (uint8_t)(n / sizeof(pulleys::MeshConfigRow));

    if (seeded) {
        pulleys::palette_load();
        Serial.printf("  [INSTALL] %u sensors mapped (stored)\n", s_count);
    } else {
        // Nothing stored: start from what the repo says, so a board swapped in
        // as arbiter does not broadcast an install nobody chose.
        s_count = 0;
        for (uint8_t i = 0; i < pulleys::CHANNEL_ASSIGNMENT_COUNT &&
                            s_count < pulleys::MESH_CONFIG_MAX_ROWS; i++) {
            s_rows[s_count].id      = pulleys::CHANNEL_ASSIGNMENT[i].id;
            s_rows[s_count].channel = pulleys::CHANNEL_ASSIGNMENT[i].channel;
            s_count++;
        }
        saveMap();
        pulleys::palette_save();
        Serial.printf("  [INSTALL] seeded %u sensors from CHANNEL_ASSIGNMENT\n", s_count);
    }
    // Announce on the first tick rather than waiting out a repeat. Not by
    // marking it dirty: nothing changed, so there is nothing to save.
    s_lastSend = millis() - REPEAT_MS;
}

void installcfg_tick() {
    uint32_t now = millis();
    if (s_dirty) {
        saveMap();
        pulleys::palette_save();
        pulleys::mesh_send_config(pulleys::palette_get(), s_rows, s_count, /*urgent=*/true);
        s_dirty    = false;
        s_lastSend = now;
    } else if (now - s_lastSend >= REPEAT_MS) {
        pulleys::mesh_send_config(pulleys::palette_get(), s_rows, s_count, /*urgent=*/false);
        s_lastSend = now;
    }
}

int installcfg_channel_of(uint16_t id) {
    int i = findRow(id);
    return i < 0 ? -1 : s_rows[i].channel;
}

bool installcfg_set_channel(uint16_t id, uint8_t channel) {
    if (channel >= pulleys::CHANNEL_COUNT) return false;
    int i = findRow(id);
    if (i < 0) {
        if (s_count >= pulleys::MESH_CONFIG_MAX_ROWS) return false;
        i = s_count++;
        s_rows[i].id = id;
    } else if (s_rows[i].channel == channel) {
        return true;
    }
    s_rows[i].channel = channel;
    s_dirty = true;
    Serial.printf("  [INSTALL] N-%04X -> ch%d\n", id, channel);
    return true;
}

bool installcfg_unmap(uint16_t id) {
    int i = findRow(id);
    if (i < 0) return false;
    s_rows[i] = s_rows[--s_count];
    s_dirty = true;
    Serial.printf("  [INSTALL] N-%04X unmapped (it keeps the channel it has)\n", id);
    return true;
}

bool installcfg_set_color(uint8_t channel, uint8_t color) {
    if (!pulleys::channel_set_color(channel, color)) return false;
    s_dirty = true;
    Serial.printf("  [INSTALL] ch%d -> %s\n", channel, pulleys::color_name(color));
    return true;
}

void installcfg_shuffle_game1() {
    const uint8_t N = pulleys::COLOR_GAME1_COUNT;
    uint8_t perm[N];
    // Fisher-Yates, redrawn until something actually moves: a shuffle that
    // leaves the day's colours where they were reads as the button not working.
    bool moved = false;
    while (!moved) {
        for (uint8_t i = 0; i < N; i++) perm[i] = i;
        for (uint8_t i = N - 1; i > 0; i--) {
            uint8_t j = (uint8_t)random(i + 1);
            uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (uint8_t i = 0; i < N; i++) if (perm[i] != i) moved = true;
    }
    uint8_t next[pulleys::CHANNEL_COUNT];
    memcpy(next, pulleys::palette_get(), sizeof(next));
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT; ch++)
        if (next[ch] < N) next[ch] = perm[next[ch]];
    if (pulleys::palette_set(next)) s_dirty = true;
    Serial.print("  [INSTALL] game1 colours shuffled:");
    for (uint8_t i = 0; i < N; i++)
        Serial.printf(" %s->%s", pulleys::color_name(i), pulleys::color_name(perm[i]));
    Serial.println();
}

uint8_t                       installcfg_row_count() { return s_count; }
const pulleys::MeshConfigRow* installcfg_rows()      { return s_rows; }

// ── Serial ────────────────────────────────────────────────────────────────────
static void printMap() {
    Serial.printf("  [INSTALL] %u sensors — paste between ASSIGNMENTS BEGIN/END in pulleys_install.h:\n",
                  s_count);
    for (uint8_t i = 0; i < s_count; i++) {
        int8_t m = pulleys::mode_for_device(s_rows[i].id);
        Serial.printf("    { 0x%04X, %2d, %s },   // N-%04X\n",
                      s_rows[i].id, s_rows[i].channel,
                      m == pulleys::ROT ? "ROT" : "LIN", s_rows[i].id);
    }
}

static void printPalette() {
    Serial.println("  [INSTALL] palette:");
    for (uint8_t ch = 1; ch < pulleys::CHANNEL_COUNT; ch++) {
        uint8_t ropes = 0;
        for (uint8_t i = 0; i < s_count; i++) if (s_rows[i].channel == ch) ropes++;
        if (ropes == 0 && ch > 24) continue;   // keep the unused tail out of the way
        Serial.printf("    ch%-2d %-6s %u rope%s\n", ch,
                      pulleys::color_name(pulleys::channel_color_index(ch)),
                      ropes, ropes == 1 ? "" : "s");
    }
}

static int parseColor(const char* s) {
    while (*s == ' ') s++;
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; c++)
        if (strcasecmp(s, pulleys::color_name(c)) == 0) return c;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    return (end != s && *end == 0 && v >= 0 && v < pulleys::COLOR_COUNT) ? (int)v : -1;
}

bool installcfg_handle_serial(const char* line) {
    if (strcmp(line, "MAP") == 0) { printMap(); return true; }
    if (strncmp(line, "MAP ", 4) == 0) {
        char* end = nullptr;
        unsigned long id = strtoul(line + 4, &end, 16);
        while (end && *end == ' ') end++;
        if (!end || end == line + 4 || id > 0xFFFF || !*end) {
            Serial.println("  [INSTALL] usage: MAP <hexid> <ch>  |  MAP <hexid> -");
        } else if (*end == '-') {
            if (!installcfg_unmap((uint16_t)id)) Serial.println("  [INSTALL] not mapped");
        } else {
            int ch = atoi(end);
            if (ch < 0 || ch >= pulleys::CHANNEL_COUNT || !installcfg_set_channel((uint16_t)id, ch))
                Serial.printf("  [INSTALL] refused (channel 0-%d, at most %d sensors)\n",
                              pulleys::CHANNEL_COUNT - 1, pulleys::MESH_CONFIG_MAX_ROWS);
        }
        return true;
    }
    if (strcmp(line, "COLOR") == 0) { printPalette(); return true; }
    if (strncmp(line, "COLOR ", 6) == 0) {
        char* end = nullptr;
        long ch = strtol(line + 6, &end, 10);
        int color = end ? parseColor(end) : -1;
        if (end == line + 6 || ch < 0 || ch >= pulleys::CHANNEL_COUNT || color < 0)
            Serial.println("  [INSTALL] usage: COLOR <ch> <red|blue|green|yellow|orange|purple>");
        else
            installcfg_set_color((uint8_t)ch, (uint8_t)color);
        return true;
    }
    if (strcmp(line, "SHUFFLE") == 0) { installcfg_shuffle_game1(); return true; }
    return false;
}
