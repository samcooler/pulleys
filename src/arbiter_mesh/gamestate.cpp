#include "gamestate.h"
#include "components.h"
#include "devices.h"
#include <ArduinoJson.h>
#include <pulleys_mesh.h>
#include <pulleys_channel.h>
#include <utility>

#define GAME_PREFIX "  [GAME]"

static const uint32_t DEFAULT_DELAY_MS = 2 * 60 * 1000;   // GAME_FORMAT.md: 2m

// default_decay_rate, when the config does not give one: a full charge lasts 15 s.
// MAX is what the packet's 16 bits can carry (thousandths of a percent per second).
static const float DEFAULT_DECAY_RATE = 100.0f / 15.0f;
static const float MAX_DECAY_RATE = 65.0f;
// charge_per_pull, when the config does not give one: six quick pulls (two a
// second) fill a component, and one pull on its own never does.
static const float DEFAULT_CHARGE_PER_PULL = 20.0f;

static JsonDocument s_doc;
static bool         s_ready = false;
static char         s_game[48]  = "";
static char         s_state[32] = "";
static uint32_t     s_enteredAt = 0;
static float        s_decayRate = DEFAULT_DECAY_RATE;
static float        s_chargePerPull = DEFAULT_CHARGE_PER_PULL;
// Majors go by colour: whichever ropes wear a game1 colour today drive that
// colour's component, so recolouring channels on the arbiter re-deals the game.
// (Finals are the purple channels, and need nothing from the config.)
static int8_t       s_majorOfColor[pulleys::COLOR_COUNT];      // colour -> major 0-4, -1 = none
static uint8_t      s_majorColor[GAME_MAJOR_COMPONENTS];       // major -> colour

static bool     s_timerOn = false;
static uint32_t s_timerAt = 0;
static char     s_timerGoto[32] = "";

// The state's `idle` block: its cues fire after `delay` with no event, then
// again every `delay` until one arrives. Any event delivered to the machine
// counts, listed by the state or not: a pull is a pull.
static bool     s_idleOn = false;
static uint32_t s_idleMs = 0;
static uint32_t s_idleAt = 0;

// The state's `at` block: timed steps, each run once, `delay` after entry.
// Bit i is entry i of the block, in file order.
static const uint8_t MAX_AT = 16;
static uint16_t s_atDone = 0;
static uint32_t s_enters = 0;   // bumps on every state entry: "did that just move us?"

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

// Is this cue named under `cues`? Cues are sent by name, so that list is only a
// check that a state does not fire something the media was never made for.
static bool isListed(JsonDocument& doc, const char* name) {
    for (JsonPair group : doc["cues"].as<JsonObject>())
        for (JsonVariant cue : group.value().as<JsonArray>())
            if (strcmp(cue.as<const char*>(), name) == 0) return true;
    return false;
}

// Is this a game the machine can run? Mirrors tools/game_to_json.py, because
// a config can also arrive by POST without passing through it.
// A colour by its pulleys_channel name, or -1.
static int colorOfName(const char* name) {
    if (!name) return -1;
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; c++)
        if (strcmp(name, pulleys::color_name(c)) == 0) return c;
    return -1;
}

static bool validate(JsonDocument& doc, String& why) {
    if (!doc["game"].is<const char*>())  { why = "missing 'game'";  return false; }
    if (!doc["start"].is<const char*>()) { why = "missing 'start'"; return false; }
    JsonObject states = doc["states"].as<JsonObject>();
    JsonObject cues   = doc["cues"].as<JsonObject>();
    if (states.isNull() || states.size() == 0) { why = "missing 'states'"; return false; }
    if (cues.isNull())                         { why = "missing 'cues'";   return false; }

    // A cue goes out on the air as its name, so the name has to fit a packet.
    for (JsonPair group : cues) {
        for (JsonVariant cue : group.value().as<JsonArray>()) {
            const char* n = cue.as<const char*>();
            if (!n || !*n || strlen(n) > pulleys::MESH_CUE_NAME_MAX) {
                why = String("cues.") + group.key().c_str() + ": a cue name is empty or longer than " +
                      pulleys::MESH_CUE_NAME_MAX + " characters";
                return false;
            }
        }
    }

    // components: five majors, each a game1 colour. No finals: those are
    // whichever channels are purple.
    JsonObject comps = doc["components"].as<JsonObject>();
    if (comps.isNull()) { why = "missing 'components'"; return false; }
    {
        JsonArray list = comps["major"].as<JsonArray>();
        if (list.isNull() || list.size() != GAME_MAJOR_COMPONENTS) {
            why = String("components.major must list exactly ") + GAME_MAJOR_COMPONENTS + " colours";
            return false;
        }
        uint8_t usedColors = 0;
        for (JsonVariant v : list) {
            int c = colorOfName(v.as<const char*>());
            if (c < 0 || c >= pulleys::COLOR_GAME1_COUNT) {
                why = "components.major: every entry must be a game1 colour (red, blue, green, yellow, orange)";
                return false;
            }
            if (usedColors & (1u << c)) {
                why = String("components.major: ") + pulleys::color_name(c) + " is listed twice";
                return false;
            }
            usedColors |= 1u << c;
        }
    }
    if (!comps["final"].isNull()) {
        why = "components.final is gone: every purple channel is a final now (set on the arbiter's COLOURS screen)";
        return false;
    }

    if (!doc["default_decay_rate"].isNull()) {
        float rate = doc["default_decay_rate"] | 0.0f;
        if (!doc["default_decay_rate"].is<float>() || rate <= 0.0f || rate > MAX_DECAY_RATE) {
            why = "default_decay_rate must be a number above 0 and at most 65 (percent per second)";
            return false;
        }
    }
    if (!doc["charge_per_pull"].isNull()) {
        float add = doc["charge_per_pull"] | 0.0f;
        if (!doc["charge_per_pull"].is<float>() || add <= 0.0f || add > 100.0f) {
            why = "charge_per_pull must be a number above 0 and at most 100 (percent of full charge)";
            return false;
        }
    }

    if (!devices_validate(doc, why)) return false;

    if (states[doc["start"].as<const char*>()].isNull()) {
        why = String("start '") + doc["start"].as<const char*>() + "' is not a state";
        return false;
    }

    // An entry in a cue list is a name, or {choose_one: [names]}.
    auto badCue = [&](const String& where, JsonVariant c) {
        if (!c.is<const char*>() || !isListed(doc, c.as<const char*>())) {
            why = where + ": cue '" + (c.is<const char*>() ? c.as<const char*>() : "?") +
                  "' is not listed under cues";
            return true;
        }
        return false;
    };
    auto badCues = [&](const String& where, JsonVariant list) {
        for (JsonVariant c : list.as<JsonArray>()) {
            if (c.is<JsonObject>()) {
                JsonVariant options = c["choose_one"];
                if (!options.is<JsonArray>() || options.size() == 0) {
                    why = where + ": choose_one must be a list of at least one cue";
                    return true;
                }
                for (JsonVariant o : options.as<JsonArray>())
                    if (badCue(where + ".choose_one", o)) return true;
            } else if (badCue(where, c)) {
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
        for (JsonPair ev : state["on"].as<JsonObject>()) {
            String where = name + ".on." + ev.key().c_str();
            if (!ev.value().isNull() && !ev.value().is<JsonObject>()) {
                why = where + ": must be a map with cue, set and/or goto";
                return false;
            }
            // A dotted event is a device's input, which the device has to declare.
            if (strchr(ev.key().c_str(), '.') && !devices_is_input(doc, ev.key().c_str())) {
                why = where + ": not an input any device emits (devices.<device>.emits)";
                return false;
            }
            if (badCues(where, ev.value()["cue"])) return false;
            if (!devices_check_set(doc, ev.value()["set"], where, why)) return false;
            if (!ev.value()["goto"].isNull() &&
                badGoto(where, ev.value()["goto"].as<const char*>())) return false;
        }
        JsonVariant idle = state["idle"];
        if (!idle.isNull()) {
            uint32_t ms;
            if (!(idle["delay"].is<const char*>() && parseDelay(idle["delay"], ms)) || ms == 0) {
                why = name + ".idle: delay must look like 10s or 2m, above 0";
                return false;
            }
            if (badCues(name + ".idle", idle["cue"])) return false;
            if (!devices_check_set(doc, idle["set"], name + ".idle", why)) return false;
        }
        JsonVariant at = state["at"];
        if (!at.isNull()) {
            if (!at.is<JsonObject>() || at.size() > MAX_AT) {
                why = name + ".at: must be a map of up to " + MAX_AT + " delays (10s: {set, cue})";
                return false;
            }
            for (JsonPair step : at.as<JsonObject>()) {
                String where = name + ".at." + step.key().c_str();
                uint32_t ms;
                if (!parseDelay(step.key().c_str(), ms)) { why = where + ": must look like 10s or 2m"; return false; }
                if (!step.value().is<JsonObject>()) { why = where + ": must be a map with cue and/or set"; return false; }
                if (!step.value()["goto"].isNull()) { why = where + ": no goto here; use after"; return false; }
                if (badCues(where, step.value()["cue"])) return false;
                if (!devices_check_set(doc, step.value()["set"], where, why)) return false;
            }
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
// `enter` marks cues a state fires on entry; the players treat those as
// interrupting (see MESH_CUE_FLAG_ENTER).
static void fire(JsonVariant cues, bool enter = false) {
    bool randomizedGoals = false;
    uint8_t flags = enter ? pulleys::MESH_CUE_FLAG_ENTER : 0;
    for (JsonVariant c : cues.as<JsonArray>()) {
        if (c.is<JsonObject>()) {   // {choose_one: [...]}: one of them, at random
            JsonArray options = c["choose_one"].as<JsonArray>();
            c = options[(size_t)random(options.size())];
        }
        const char* name = c.as<const char*>();
        randomizedGoals = components_on_cue(name) || randomizedGoals;
        if (!pulleys::mesh_send_cue(name, flags)) continue;   // validated on load
        record('C', "%s", name);
    }
    if (randomizedGoals) components_check_simon_complete();
}

// One reaction's actions: its settings first, then its cues. Settings first so
// a device and a clip triggered together see the room already changed.
static void act(JsonVariant block, bool enter = false) {
    for (JsonPair kv : block["set"].as<JsonObject>()) {
        devices_set(kv.key().c_str(), kv.value().as<const char*>());
        record('C', "set %s=%s", kv.key().c_str(), kv.value().as<const char*>());
    }
    fire(block["cue"], enter);
}

static void enter(const char* name) {
    char from[32];
    strlcpy(from, s_state[0] ? s_state : "-", sizeof(from));
    strlcpy(s_state, name, sizeof(s_state));
    s_enteredAt = millis();
    s_timerOn = false;
    s_idleOn  = false;
    s_atDone  = 0;
    s_enters++;

    if (from[0] == '-') record('S', "start: %s", s_state);
    else record('S', "%s -> %s", from, s_state);

    JsonVariant state = s_doc["states"][name];
    JsonVariant after = state["after"];
    if (!after.isNull()) {
        uint32_t ms = DEFAULT_DELAY_MS;
        if (!after["delay"].isNull()) parseDelay(after["delay"], ms);
        strlcpy(s_timerGoto, after["goto"].as<const char*>(), sizeof(s_timerGoto));
        s_timerAt = s_enteredAt + ms;
        s_timerOn = true;
    }
    JsonVariant idle = state["idle"];
    if (!idle.isNull()) {
        parseDelay(idle["delay"], s_idleMs);   // validated on load
        s_idleAt = s_enteredAt + s_idleMs;
        s_idleOn = true;
    }

    // State entry is represented in the YAML like other reactions, under
    // `on: enter:`. Set its timer first: an entry cue may complete Simon's
    // current goal set and synchronously move the state machine again.
    act(state["on"]["enter"], true);
}

bool gamestate_load(const String& json, String& why) {
    JsonDocument next;
    DeserializationError err = deserializeJson(next, json);
    if (err) { why = String("not valid JSON: ") + err.c_str(); return false; }
    if (!validate(next, why)) return false;

    s_doc = std::move(next);
    s_ready = true;
    strlcpy(s_game, s_doc["game"].as<const char*>(), sizeof(s_game));
    s_decayRate = s_doc["default_decay_rate"] | DEFAULT_DECAY_RATE;
    s_chargePerPull = s_doc["charge_per_pull"] | DEFAULT_CHARGE_PER_PULL;
    memset(s_majorOfColor, -1, sizeof(s_majorOfColor));
    {
        int8_t n = 0;
        for (JsonVariant v : s_doc["components"]["major"].as<JsonArray>()) {
            uint8_t c = (uint8_t)colorOfName(v.as<const char*>());   // validated above
            s_majorOfColor[c] = n;
            s_majorColor[n++] = c;
        }
    }
    s_state[0] = 0;
    s_timerOn = false;
    s_idleOn  = false;
    devices_load(s_doc);
    Serial.printf(GAME_PREFIX " loaded '%s'\n", s_game);
    enter(s_doc["start"].as<const char*>());
    return true;
}

void gamestate_event(GameEvent e) { gamestate_input(gameevent_name(e)); }

void gamestate_input(const char* name) {
    if (!s_ready) return;
    if (s_idleOn) s_idleAt = millis() + s_idleMs;   // any event is activity
    JsonVariant state = s_doc["states"][s_state];
    JsonObject on = state["on"].as<JsonObject>();
    if (!hasKey(on, name)) {
        // Most events are ignored in most states; the panel log would drown.
        Serial.printf(GAME_PREFIX " . %s ignored in %s\n", name, s_state);
        return;
    }
    record('E', "%s", name);
    JsonVariant action = on[name];
    act(action);
    if (!action["goto"].isNull()) enter(action["goto"].as<const char*>());
}

void gamestate_tick() {
    if (!s_ready) return;
    if (s_timerOn && (int32_t)(millis() - s_timerAt) >= 0) {
        char target[32];
        strlcpy(target, s_timerGoto, sizeof(target));
        record('T', "timer in %s", s_state);
        enter(target);
        return;
    }
    if (s_idleOn && (int32_t)(millis() - s_idleAt) >= 0) {
        s_idleAt = millis() + s_idleMs;   // before fire: a cue may change state
        record('T', "idle in %s", s_state);
        uint32_t enters = s_enters;
        act(s_doc["states"][s_state]["idle"]);
        if (s_enters != enters) return;
    }
    uint8_t i = 0;
    uint32_t age = millis() - s_enteredAt;
    for (JsonPair step : s_doc["states"][s_state]["at"].as<JsonObject>()) {
        uint16_t bit = (uint16_t)1 << i++;
        uint32_t ms;
        if ((s_atDone & bit) || !parseDelay(step.key().c_str(), ms) || age < ms) continue;
        s_atDone |= bit;
        record('T', "at %s in %s", step.key().c_str(), s_state);
        uint32_t enters = s_enters;
        act(step.value());
        if (s_enters != enters) return;   // that step's cue moved the machine on
    }
}

bool        gamestate_ready()  { return s_ready; }
const char* gamestate_game()   { return s_game; }
const char* gamestate_state()  { return s_state; }
float       gamestate_decay_rate() { return s_decayRate; }
float       gamestate_charge_per_pull() { return s_chargePerPull; }
int         gamestate_major_for_channel(uint8_t channel) {
    if (!s_ready || channel >= pulleys::CHANNEL_COUNT) return -1;
    return s_majorOfColor[pulleys::channel_color_index(channel)];
}

uint8_t     gamestate_major_color(uint8_t major) {
    if (!s_ready || major >= GAME_MAJOR_COMPONENTS) return pulleys::MESH_COLOR_UNKNOWN;
    return s_majorColor[major];
}

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
