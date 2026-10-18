#include "gamestate.h"
#include <ArduinoJson.h>
#include <pulleys_mesh.h>
#include <pulleys_cues.h>
#include <utility>

#define GAME_PREFIX "  [GAME]"

static const uint32_t DEFAULT_DELAY_MS = 2 * 60 * 1000;   // GAME_FORMAT.md: 2m

static JsonDocument s_doc;
static bool         s_ready = false;
static char         s_game[48]  = "";
static char         s_state[32] = "";
static uint16_t     s_hash = 0;
static uint32_t     s_enteredAt = 0;

static bool     s_timerOn = false;
static uint32_t s_timerAt = 0;
static char     s_timerGoto[32] = "";

static GameTrigger s_log[GAME_LOG_LEN];
static uint8_t     s_logCount = 0;
static uint32_t    s_seq = 0;

// ── Log ───────────────────────────────────────────────────────────────────────
static void record(char kind, const char* fmt, const char* a = "", const char* b = "") {
    for (int i = GAME_LOG_LEN - 1; i > 0; i--) s_log[i] = s_log[i - 1];
    s_log[0].atMs = millis();
    s_log[0].kind = kind;
    snprintf(s_log[0].text, sizeof(s_log[0].text), fmt, a, b);
    if (s_logCount < GAME_LOG_LEN) s_logCount++;
    s_seq++;
    Serial.printf(GAME_PREFIX " %c %s\n", kind, s_log[0].text);
}

// ── Config helpers ────────────────────────────────────────────────────────────
static bool hasKey(JsonObjectConst o, const char* key) {
    for (JsonPairConst kv : o) if (strcmp(kv.key().c_str(), key) == 0) return true;
    return false;
}

// "10s" or "2m" -> ms. Whole units only, as GAME_FORMAT.md says.
static bool parseDelay(const char* text, uint32_t& ms) {
    char* end = nullptr;
    long n = strtol(text, &end, 10);
    if (end == text || n < 0) return false;
    if (strcmp(end, "s") == 0) { ms = (uint32_t)n * 1000UL;  return true; }
    if (strcmp(end, "m") == 0) { ms = (uint32_t)n * 60000UL; return true; }
    return false;
}

// Where a cue sits in the table: folder = its group's position, file = its
// position inside the group, both from 0 (the cue index in GAME_FORMAT.md).
static bool findCue(JsonDocument& doc, const char* name, uint8_t& folder, uint8_t& file) {
    uint16_t g = 0;
    for (JsonPair group : doc["cues"].as<JsonObject>()) {
        uint16_t f = 0;
        for (JsonVariant cue : group.value().as<JsonArray>()) {
            if (strcmp(cue.as<const char*>(), name) == 0) {
                if (g > 255 || f > 255) return false;
                folder = (uint8_t)g;
                file   = (uint8_t)f;
                return true;
            }
            f++;
        }
        g++;
    }
    return false;
}

static uint16_t hashOf(JsonDocument& doc) {
    uint32_t h = pulleys::cues_hash_begin();
    for (JsonPair group : doc["cues"].as<JsonObject>()) {
        h = pulleys::cues_hash_add(h, group.key().c_str(), 1);
        for (JsonVariant cue : group.value().as<JsonArray>())
            h = pulleys::cues_hash_add(h, cue.as<const char*>(), 0);
    }
    return pulleys::cues_hash_end(h);
}

// Is this a game the machine can run? Mirrors tools/game_to_json.py, because
// a config can also arrive by POST without passing through it.
static bool validate(JsonDocument& doc, String& why) {
    if (!doc["game"].is<const char*>())  { why = "missing 'game'";  return false; }
    if (!doc["start"].is<const char*>()) { why = "missing 'start'"; return false; }
    JsonObject states = doc["states"].as<JsonObject>();
    JsonObject cues   = doc["cues"].as<JsonObject>();
    if (states.isNull() || states.size() == 0) { why = "missing 'states'"; return false; }
    if (cues.isNull())                         { why = "missing 'cues'";   return false; }
    if (cues.size() > 256) { why = "more than 256 cue groups"; return false; }

    for (JsonPair group : cues) {
        if (group.value().as<JsonArray>().size() > 256) {
            why = String("cues.") + group.key().c_str() + ": more than 256 cues";
            return false;
        }
    }

    if (states[doc["start"].as<const char*>()].isNull()) {
        why = String("start '") + doc["start"].as<const char*>() + "' is not a state";
        return false;
    }

    auto badCues = [&](const String& where, JsonVariant list) {
        uint8_t fo, fi;
        for (JsonVariant c : list.as<JsonArray>()) {
            if (!c.is<const char*>() || !findCue(doc, c.as<const char*>(), fo, fi)) {
                why = where + ": cue '" + (c.is<const char*>() ? c.as<const char*>() : "?") +
                      "' is not listed under cues";
                return true;
            }
        }
        return false;
    };
    auto badGoto = [&](const String& where, const char* target) {
        if (!target || states[target].isNull()) {
            why = where + ": goto '" + (target ? target : "") + "' is not a state";
            return true;
        }
        return false;
    };

    for (JsonPair st : states) {
        String name = st.key().c_str();
        JsonVariant state = st.value();
        if (badCues(name + ".on_enter_cue", state["on_enter_cue"])) return false;
        for (JsonPair ev : state["on"].as<JsonObject>()) {
            String where = name + ".on." + ev.key().c_str();
            if (badCues(where, ev.value()["cue"])) return false;
            if (!ev.value()["goto"].isNull() &&
                badGoto(where, ev.value()["goto"].as<const char*>())) return false;
        }
        JsonVariant after = state["after"];
        if (!after.isNull()) {
            if (badGoto(name + ".after", after["goto"].as<const char*>())) return false;
            uint32_t ms;
            if (!after["delay"].isNull() &&
                !(after["delay"].is<const char*>() && parseDelay(after["delay"], ms))) {
                why = name + ".after: delay must look like 10s or 2m";
                return false;
            }
        }
    }
    return true;
}

// ── The machine ───────────────────────────────────────────────────────────────
static void fire(JsonVariant cues) {
    for (JsonVariant c : cues.as<JsonArray>()) {
        const char* name = c.as<const char*>();
        uint8_t folder, file;
        if (!findCue(s_doc, name, folder, file)) continue;   // validated on load
        pulleys::mesh_send_cue(folder, file, s_hash);
        record('C', "%s", name);
    }
}

static void enter(const char* name) {
    char from[32];
    strlcpy(from, s_state[0] ? s_state : "-", sizeof(from));
    strlcpy(s_state, name, sizeof(s_state));
    s_enteredAt = millis();
    s_timerOn = false;

    if (from[0] == '-') record('S', "start: %s", s_state);
    else record('S', "%s -> %s", from, s_state);

    JsonVariant state = s_doc["states"][name];
    fire(state["on_enter_cue"]);

    JsonVariant after = state["after"];
    if (!after.isNull()) {
        uint32_t ms = DEFAULT_DELAY_MS;
        if (!after["delay"].isNull()) parseDelay(after["delay"], ms);
        strlcpy(s_timerGoto, after["goto"].as<const char*>(), sizeof(s_timerGoto));
        s_timerAt = s_enteredAt + ms;
        s_timerOn = true;
    }
}

bool gamestate_load(const String& json, String& why) {
    JsonDocument next;
    DeserializationError err = deserializeJson(next, json);
    if (err) { why = String("not valid JSON: ") + err.c_str(); return false; }
    if (!validate(next, why)) return false;

    s_doc = std::move(next);
    s_ready = true;
    s_hash = hashOf(s_doc);
    strlcpy(s_game, s_doc["game"].as<const char*>(), sizeof(s_game));
    s_state[0] = 0;
    s_timerOn = false;
    Serial.printf(GAME_PREFIX " loaded '%s' (cue hash %04X)\n", s_game, s_hash);
    enter(s_doc["start"].as<const char*>());
    return true;
}

void gamestate_event(GameEvent e) {
    if (!s_ready) return;
    const char* name = gameevent_name(e);
    JsonVariant state = s_doc["states"][s_state];
    JsonObject on = state["on"].as<JsonObject>();
    if (!hasKey(on, name)) {
        // Most events are ignored in most states; the panel log would drown.
        Serial.printf(GAME_PREFIX " . %s ignored in %s\n", name, s_state);
        return;
    }
    record('E', "%s", name);
    JsonVariant action = on[name];
    fire(action["cue"]);
    if (!action["goto"].isNull()) enter(action["goto"].as<const char*>());
}

void gamestate_tick() {
    if (!s_ready || !s_timerOn) return;
    if ((int32_t)(millis() - s_timerAt) < 0) return;
    char target[32];
    strlcpy(target, s_timerGoto, sizeof(target));
    record('T', "timer in %s", s_state);
    enter(target);
}

bool        gamestate_ready()  { return s_ready; }
const char* gamestate_game()   { return s_game; }
const char* gamestate_state()  { return s_state; }
uint16_t    gamestate_cue_hash() { return s_hash; }

uint32_t gamestate_state_age_ms() { return millis() - s_enteredAt; }

int32_t gamestate_timer_left_ms() {
    if (!s_timerOn) return -1;
    int32_t left = (int32_t)(s_timerAt - millis());
    return left < 0 ? 0 : left;
}
const char* gamestate_timer_target() { return s_timerOn ? s_timerGoto : ""; }

const GameTrigger* gamestate_log()       { return s_log; }
uint8_t            gamestate_log_count() { return s_logCount; }
uint32_t           gamestate_trigger_seq() { return s_seq; }
