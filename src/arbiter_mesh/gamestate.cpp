#include "gamestate.h"
#include "components.h"
#include "devices.h"
#include "installcfg.h"
#include <ArduinoJson.h>
#include <pulleys_mesh.h>
#include <pulleys_channel.h>
#include <math.h>
#include <stdarg.h>
#include <utility>

#define GAME_PREFIX "  [GAME]"

// The Python engine (playtest/engine.py) runs the same rules on a virtual
// clock, for the playtest simulator; playtest/tests/test_engine.py holds the
// behaviours the two must share. Change one, change the other.

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

// The state's `every` block: runs every `delay` from entry, whatever happens.
static bool     s_everyOn = false;
static uint32_t s_everyMs = 0;
static uint32_t s_everyAt = 0;

// The state's `at` block: timed steps, each run once, `delay` after entry.
// Bit i is entry i of the block, in file order.
static const uint8_t MAX_AT = 16;
static uint16_t s_atDone = 0;
static uint32_t s_enters = 0;   // bumps on every state entry: "did that just move us?"

static GameTrigger s_log[GAME_LOG_LEN];
static uint8_t     s_logCount = 0;
static uint32_t    s_seq = 0;

// ── Params ────────────────────────────────────────────────────────────────────
// Named values a state uses as `$name` wherever a delay or a count goes. The
// JSON carries plain numbers (tools/game_to_json.py turns `2m` into 120), so
// here a param is a float: seconds for a duration. One with a range may be
// set live, clamped to it; one without is a constant.
static const uint8_t MAX_PARAMS = 16;
struct Param {
    char  name[24];
    float value, lo, hi;
    bool  ranged;
};
static Param   s_params[MAX_PARAMS];
static uint8_t s_paramCount = 0;
// Where `$name` is looked up: the running game's table -- or, while a new
// config is being checked, that config's own.
static const Param* s_look  = s_params;
static uint8_t      s_lookN = 0;
// default_decay_rate and charge_per_pull: a number, or the param they name.
static float s_decayRate = DEFAULT_DECAY_RATE;
static int8_t s_decayParam = -1;
static float s_chargePerPull = DEFAULT_CHARGE_PER_PULL;
static int8_t s_chargeParam = -1;

// ── Counters ──────────────────────────────────────────────────────────────────
// Whole numbers a state adds to (`add:`) and clears (`clear:`). One with a
// `reach` sends <name>_reached when it climbs to that mark.
static const uint8_t MAX_COUNTERS = 8;
struct Counter {
    char             name[16];
    int32_t          value;
    JsonVariantConst reach;   // into s_doc; null if it has none
};
static Counter s_counters[MAX_COUNTERS];
static uint8_t s_counterCount = 0;

// ── Telemetry (MESH_MSG_GAME_INFO) ───────────────────────────────────────────
// Space-separated key=value items, gathered through one pass of the loop and
// sent as one flood at the end of gamestate_tick(), so a state change with its
// goals and a score is one packet, not three. The Pi's play log reads them
// (overlord/playlog.py). Best effort: a lost one costs a line of a log.
static char     s_info[pulleys::MESH_FLOOD_MAX_BODY + 1];
static uint16_t s_infoLen = 0;

static void infoFlush(uint8_t copies = 2) {
    if (!s_infoLen) return;
    pulleys::mesh_send_flood(pulleys::MESH_MSG_GAME_INFO, s_info, (uint8_t)s_infoLen, copies, 120);
    s_infoLen = 0;
}

static void info(const char* fmt, ...) {
    char item[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(item, sizeof(item), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(item)) n = sizeof(item) - 1;
    for (int i = 0; i < n; i++) if (item[i] == ' ') item[i] = '-';   // spaces separate items
    if (s_infoLen + 1 + n > pulleys::MESH_FLOOD_MAX_BODY) infoFlush();
    if (s_infoLen) s_info[s_infoLen++] = ' ';
    memcpy(s_info + s_infoLen, item, n);
    s_infoLen += n;
    s_info[s_infoLen] = 0;
}

static long milli(float v) { return lroundf(v * 1000.0f); }

static void infoGoals(const char* key, uint8_t mask) {
    char names[48] = "";
    for (uint8_t i = 0; i < GAME_MAJOR_COMPONENTS; i++) {
        if (!(mask & (1u << i))) continue;
        if (names[0]) strlcat(names, "-", sizeof(names));
        strlcat(names, pulleys::color_name(s_majorColor[i]), sizeof(names));
    }
    info("%s=%s", key, names[0] ? names : "none");
}

static void infoPalette(const char* key) {
    char digits[pulleys::CHANNEL_COUNT + 1];
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT; ch++)
        digits[ch] = (char)('0' + pulleys::channel_color_index(ch) % 10);
    digits[pulleys::CHANNEL_COUNT] = 0;
    info("%s=%s", key, digits);
}

void gamestate_note_goals(uint8_t mask) { if (s_ready) infoGoals("goals", mask); }
void gamestate_note_palette()           { if (s_ready) infoPalette("palette"); }

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

static int findIn(const Param* t, uint8_t n, const char* name) {
    for (uint8_t i = 0; i < n; i++) if (strcmp(t[i].name, name) == 0) return i;
    return -1;
}

// `$name` -> its value, from wherever lookups point now.
static bool lookup(const char* ref, float& out) {
    if (!ref || ref[0] != '$') return false;
    int i = findIn(s_look, s_lookN, ref + 1);
    if (i < 0) return false;
    out = s_look[i].value;
    return true;
}

// "10s", "1.5s" or "2m" -> ms.
static bool parseDelayText(const char* text, uint32_t& ms) {
    if (!text) return false;
    char* end = nullptr;
    float n = strtof(text, &end);
    if (end == text || !isfinite(n) || n < 0) return false;
    if (strcmp(end, "s") == 0) { ms = (uint32_t)(n * 1000.0f + 0.5f);  return true; }
    if (strcmp(end, "m") == 0) { ms = (uint32_t)(n * 60000.0f + 0.5f); return true; }
    return false;
}

// A delay: "10s", or "$name" of a param (in seconds).
static bool delayText(const char* text, uint32_t& ms) {
    if (text && text[0] == '$') {
        float s;
        if (!lookup(text, s) || !isfinite(s) || s < 0) return false;
        ms = (uint32_t)(s * 1000.0f + 0.5f);
        return true;
    }
    return parseDelayText(text, ms);
}
static bool delayOf(JsonVariantConst v, uint32_t& ms) { return delayText(v.as<const char*>(), ms); }

// A count or a rate: a number, or "$name".
static bool numberOf(JsonVariantConst v, float& out) {
    if (v.is<float>()) { out = v.as<float>(); return true; }
    return lookup(v.as<const char*>(), out);
}

// Is this cue named under `cues`? Cues are sent by name, so that list is only a
// check that a state does not fire something the media was never made for.
static bool isListed(JsonDocument& doc, const char* name) {
    for (JsonPair group : doc["cues"].as<JsonObject>())
        for (JsonVariant cue : group.value().as<JsonArray>())
            if (strcmp(cue.as<const char*>(), name) == 0) return true;
    return false;
}

// A colour by its pulleys_channel name, or -1.
static int colorOfName(const char* name) {
    if (!name) return -1;
    for (uint8_t c = 0; c < pulleys::COLOR_COUNT; c++)
        if (strcmp(name, pulleys::color_name(c)) == 0) return c;
    return -1;
}

static bool loadParams(JsonObjectConst params, Param* out, uint8_t& n, String& why) {
    n = 0;
    for (JsonPairConst kv : params) {
        const char* name = kv.key().c_str();
        if (n >= MAX_PARAMS) { why = String("params: at most ") + MAX_PARAMS; return false; }
        if (!*name || strlen(name) >= sizeof(out[n].name)) {
            why = String("params.") + name + ": a name is 1-23 characters";
            return false;
        }
        Param& p = out[n];
        strlcpy(p.name, name, sizeof(p.name));
        JsonVariantConst v = kv.value();
        if (v.is<float>()) {
            p.value = p.lo = p.hi = v.as<float>();
            p.ranged = false;
        } else if (v["value"].is<float>()) {
            p.value  = v["value"].as<float>();
            p.ranged = v["min"].is<float>() && v["max"].is<float>();
            p.lo = p.ranged ? v["min"].as<float>() : p.value;
            p.hi = p.ranged ? v["max"].as<float>() : p.value;
            if (p.ranged && !(p.lo <= p.value && p.value <= p.hi)) {
                why = String("params.") + name + ": value must lie between min and max";
                return false;
            }
        } else {
            why = String("params.") + name +
                  ": needs a number value (durations in seconds -- tools/game_to_json.py converts them)";
            return false;
        }
        n++;
    }
    return true;
}

// A `$name` or number that must lie in (lo, hi] -- and, for a param with a
// range, so must the whole range, since the director may set any of it.
static bool inRange(JsonVariantConst v, float lo, float hi) {
    const char* ref = v.as<const char*>();
    if (ref && ref[0] == '$') {
        int i = findIn(s_look, s_lookN, ref + 1);
        if (i < 0) return false;
        const Param& p = s_look[i];
        return p.value > lo && p.value <= hi && p.lo > lo && p.hi <= hi;
    }
    float x;
    return numberOf(v, x) && x > lo && x <= hi;
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

    if (!doc["default_decay_rate"].isNull() && !inRange(doc["default_decay_rate"], 0.0f, MAX_DECAY_RATE)) {
        why = "default_decay_rate must be a number (or param) above 0 and at most 65 (percent per second)";
        return false;
    }
    if (!doc["charge_per_pull"].isNull() && !inRange(doc["charge_per_pull"], 0.0f, 100.0f)) {
        why = "charge_per_pull must be a number (or param) above 0 and at most 100 (percent of full charge)";
        return false;
    }

    // counters: names that fit, and a reach that is a number or a param
    JsonObject counters = doc["counters"].as<JsonObject>();
    if (counters.size() > MAX_COUNTERS) { why = String("counters: at most ") + MAX_COUNTERS; return false; }
    for (JsonPair c : counters) {
        float x;
        if (strlen(c.key().c_str()) >= sizeof(s_counters[0].name)) {
            why = String("counters.") + c.key().c_str() + ": a name is at most 15 characters";
            return false;
        }
        if (!c.value()["reach"].isNull() && !numberOf(c.value()["reach"], x)) {
            why = String("counters.") + c.key().c_str() + ".reach: a number, or a param";
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
    // The keys a reaction may hold besides cue, set and goto.
    auto badAction = [&](const String& where, JsonVariant block) {
        for (JsonVariant c : block["clear"].as<JsonArray>()) {
            if (!c.is<const char*>() || !hasKey(counters, c.as<const char*>())) {
                why = where + ".clear: not a counter"; return true;
            }
        }
        for (JsonPair kv : block["add"].as<JsonObject>()) {
            float x;
            if (!hasKey(counters, kv.key().c_str()) || !numberOf(kv.value(), x)) {
                why = where + ".add." + kv.key().c_str() + ": a counter, and a number or param";
                return true;
            }
        }
        if (!block["goals"].isNull() && !inRange(block["goals"], 0.5f, GAME_MAJOR_COMPONENTS)) {
            why = where + ".goals: 1 to 5, or a param in that range"; return true;
        }
        const char* colors = block["colors"].as<const char*>();
        if (!block["colors"].isNull() &&
            !(colors && (strcmp(colors, "shuffle") == 0 || strcmp(colors, "restore") == 0))) {
            why = where + ".colors: shuffle or restore"; return true;
        }
        if (badCues(where, block["cue"])) return true;
        if (!devices_check_set(doc, block["set"], where, why)) return true;
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
            if (badAction(where, ev.value())) return false;
            if (!ev.value()["goto"].isNull() &&
                badGoto(where, ev.value()["goto"].as<const char*>())) return false;
        }
        for (const char* key : {"idle", "every"}) {
            JsonVariant block = state[key];
            if (block.isNull()) continue;
            String where = name + "." + key;
            uint32_t ms;
            if (!delayOf(block["delay"], ms) || ms == 0) {
                why = where + ": delay must look like 10s, 1.5s or 2m (or a param), above 0";
                return false;
            }
            if (!block["goto"].isNull()) { why = where + ": no goto here; use after"; return false; }
            if (badAction(where, block)) return false;
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
                if (!delayText(step.key().c_str(), ms)) { why = where + ": must look like 10s or 2m"; return false; }
                if (!step.value().is<JsonObject>()) { why = where + ": must be a map with cue and/or set"; return false; }
                if (!step.value()["goto"].isNull()) { why = where + ": no goto here; use after"; return false; }
                if (badAction(where, step.value())) return false;
            }
        }
        JsonVariant after = state["after"];
        if (!after.isNull()) {
            if (badGoto(name + ".after", after["goto"].as<const char*>())) return false;
            uint32_t ms;
            if (!after["delay"].isNull() && !delayOf(after["delay"], ms)) {
                why = name + ".after: delay must look like 10s, 1.5s or 2m (or a param)";
                return false;
            }
        }
    }
    return true;
}

// ── Counters ──────────────────────────────────────────────────────────────────
static int findCounter(const char* name) {
    if (!name) return -1;
    for (uint8_t i = 0; i < s_counterCount; i++)
        if (strcmp(s_counters[i].name, name) == 0) return i;
    return -1;
}

// Set a counter; mark it in `reached` if that took it up to its mark.
static void setCounter(uint8_t i, int32_t v, uint8_t& reached) {
    Counter& c = s_counters[i];
    if (v == c.value) return;
    int32_t old = c.value;
    c.value = v;
    info("count.%s=%ld", c.name, (long)v);
    float mark;
    if (!c.reach.isNull() && numberOf(c.reach, mark)) {
        int32_t m = (int32_t)lroundf(mark);
        if (old < m && m <= v) reached |= (uint8_t)(1u << i);
    }
}

// ── The machine ───────────────────────────────────────────────────────────────
static void enter(const char* name);

// `enter` marks cues a state fires on entry; the players treat those as
// interrupting (see MESH_CUE_FLAG_ENTER). True in `dealt` if a cue dealt goals.
static void fire(JsonVariant cues, bool enter, bool& dealt) {
    uint8_t flags = enter ? pulleys::MESH_CUE_FLAG_ENTER : 0;
    for (JsonVariant c : cues.as<JsonArray>()) {
        if (c.is<JsonObject>()) {   // {choose_one: [...]}: one of them, at random
            JsonArray options = c["choose_one"].as<JsonArray>();
            c = options[(size_t)random(options.size())];
        }
        const char* name = c.as<const char*>();
        dealt = components_on_cue(name) || dealt;
        if (!pulleys::mesh_send_cue(name, flags)) continue;   // validated on load
        record('C', "%s", name);
    }
}

// One reaction: set, clear, add, colors, goals, cue -- in that order, the
// same as playtest/engine.py -- then the <counter>_reached events it caused.
// The caller handles goto. Settings first, so a device and a clip triggered
// together see the room already changed.
static void act(JsonVariant block, bool enter = false) {
    for (JsonPair kv : block["set"].as<JsonObject>()) {
        devices_set(kv.key().c_str(), kv.value().as<const char*>());
        record('C', "set %s=%s", kv.key().c_str(), kv.value().as<const char*>());
    }
    uint8_t reached = 0;
    for (JsonVariant c : block["clear"].as<JsonArray>()) {
        int i = findCounter(c.as<const char*>());
        if (i >= 0) setCounter(i, 0, reached);
    }
    for (JsonPair kv : block["add"].as<JsonObject>()) {
        int i = findCounter(kv.key().c_str());
        float n;
        if (i >= 0 && numberOf(kv.value(), n))
            setCounter(i, s_counters[i].value + (int32_t)lroundf(n), reached);
    }
    const char* colors = block["colors"].as<const char*>();
    if (colors) {
        if (strcmp(colors, "shuffle") == 0) installcfg_game_shuffle();
        else                                installcfg_game_restore();
        record('C', "colors %s", colors);
    }
    bool dealt = false;
    float goals;
    if (!block["goals"].isNull() && numberOf(block["goals"], goals)) {
        components_deal_goals((uint8_t)constrain(lroundf(goals), 1L, (long)GAME_MAJOR_COMPONENTS));
        record('C', "goals %s", block["goals"].is<const char*>() ? block["goals"].as<const char*>() : "dealt");
        dealt = true;
    }
    fire(block["cue"], enter, dealt);
    // A lit set can already match goals just dealt.
    if (dealt) components_check_simon_complete();
    for (uint8_t i = 0; i < s_counterCount; i++) {
        if (!(reached & (1u << i))) continue;
        char ev[sizeof(s_counters[i].name) + 8];
        snprintf(ev, sizeof(ev), "%s_reached", s_counters[i].name);
        gamestate_input(ev);
    }
}

static void enter(const char* name) {
    char from[32];
    strlcpy(from, s_state[0] ? s_state : "-", sizeof(from));
    strlcpy(s_state, name, sizeof(s_state));
    s_enteredAt = millis();
    s_timerOn = false;
    s_idleOn  = false;
    s_everyOn = false;
    s_atDone  = 0;
    s_enters++;

    if (from[0] == '-') record('S', "start: %s", s_state);
    else record('S', "%s -> %s", from, s_state);
    info("state=%s", s_state);

    JsonVariant state = s_doc["states"][name];
    JsonVariant after = state["after"];
    if (!after.isNull()) {
        uint32_t ms = DEFAULT_DELAY_MS;
        if (!after["delay"].isNull()) delayOf(after["delay"], ms);
        strlcpy(s_timerGoto, after["goto"].as<const char*>(), sizeof(s_timerGoto));
        s_timerAt = s_enteredAt + ms;
        s_timerOn = true;
    }
    JsonVariant idle = state["idle"];
    if (!idle.isNull() && delayOf(idle["delay"], s_idleMs) && s_idleMs > 0) {
        s_idleAt = s_enteredAt + s_idleMs;
        s_idleOn = true;
    }
    JsonVariant every = state["every"];
    if (!every.isNull() && delayOf(every["delay"], s_everyMs) && s_everyMs > 0) {
        s_everyAt = s_enteredAt + s_everyMs;
        s_everyOn = true;
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

    // Check it against its own params, then adopt both or neither.
    Param params[MAX_PARAMS];
    uint8_t nParams = 0;
    if (!loadParams(next["params"].as<JsonObjectConst>(), params, nParams, why)) return false;
    const Param* look = s_look;
    uint8_t lookN = s_lookN;
    s_look = params;
    s_lookN = nParams;
    bool ok = validate(next, why);
    s_look = look;
    s_lookN = lookN;
    if (!ok) return false;

    infoFlush();   // anything the old game still had to say goes out under its name
    s_doc = std::move(next);
    memcpy(s_params, params, sizeof(Param) * nParams);
    s_paramCount = nParams;
    s_look = s_params;
    s_lookN = s_paramCount;
    s_ready = true;
    strlcpy(s_game, s_doc["game"].as<const char*>(), sizeof(s_game));

    auto knob = [](JsonVariantConst v, float fallback, float& value, int8_t& param) {
        param = -1;
        value = fallback;
        const char* ref = v.as<const char*>();
        if (ref && ref[0] == '$') param = (int8_t)findIn(s_params, s_paramCount, ref + 1);
        else if (v.is<float>()) value = v.as<float>();
    };
    knob(s_doc["default_decay_rate"], DEFAULT_DECAY_RATE, s_decayRate, s_decayParam);
    knob(s_doc["charge_per_pull"], DEFAULT_CHARGE_PER_PULL, s_chargePerPull, s_chargeParam);

    s_counterCount = 0;
    for (JsonPair c : s_doc["counters"].as<JsonObject>()) {
        Counter& k = s_counters[s_counterCount++];
        strlcpy(k.name, c.key().c_str(), sizeof(k.name));
        k.value = 0;
        k.reach = c.value()["reach"];
    }

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
    s_everyOn = false;
    installcfg_game_restore();   // a shuffle belonged to the old game
    devices_load(s_doc);
    Serial.printf(GAME_PREFIX " loaded '%s'\n", s_game);
    info("start=%s", s_game);
    for (uint8_t i = 0; i < s_paramCount; i++)
        info("param.%s=%ld", s_params[i].name, milli(s_params[i].value));
    infoPalette("palette");
    enter(s_doc["start"].as<const char*>());
    return true;
}

void gamestate_event(GameEvent e) { gamestate_input(gameevent_name(e)); }

void gamestate_input(const char* name) {
    if (!s_ready || !name || strcmp(name, "enter") == 0) return;
    if (s_idleOn) s_idleAt = millis() + s_idleMs;   // any event is activity
    JsonVariant state = s_doc["states"][s_state];
    JsonObject on = state["on"].as<JsonObject>();
    if (!hasKey(on, name)) {
        // Most events are ignored in most states; the panel log would drown.
        // Pulls come several a second, so they do not even reach the console.
        GameEvent e;
        if (!(gameevent_from_name(name, e) && gameevent_is_pull(e)))
            Serial.printf(GAME_PREFIX " . %s ignored in %s\n", name, s_state);
        return;
    }
    record('E', "%s", name);
    JsonVariant action = on[name];
    uint32_t enters = s_enters;
    act(action);
    // A reaction that already moved the machine -- a counter reaching its
    // mark, Simon completing -- wins over its own goto.
    if (!action["goto"].isNull() && s_enters == enters) enter(action["goto"].as<const char*>());
}

// Every 15 s, what the game is doing now, so a play log that started late (or
// missed a packet) catches up: hb.* items, which the Pi uses to correct what it
// thinks rather than as news. Params on alternate beats; they fill a packet.
static void heartbeat() {
    static uint32_t last = 0;
    static bool paramsTurn = false;
    if (millis() - last < 15000) return;
    last = millis();
    if (paramsTurn) {
        for (uint8_t i = 0; i < s_paramCount; i++)
            info("param.%s=%ld", s_params[i].name, milli(s_params[i].value));
    } else {
        info("hb.state=%s", s_state);
        infoGoals("hb.goals", components_goal_mask());
        infoPalette("hb.palette");
    }
    paramsTurn = !paramsTurn;
    infoFlush(1);
}

static void timers() {
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
        if ((s_atDone & bit) || !delayText(step.key().c_str(), ms) || age < ms) continue;
        s_atDone |= bit;
        record('T', "at %s in %s", step.key().c_str(), s_state);
        uint32_t enters = s_enters;
        act(step.value());
        if (s_enters != enters) return;   // that step's cue moved the machine on
    }
    if (s_everyOn && (int32_t)(millis() - s_everyAt) >= 0) {
        s_everyAt = millis() + s_everyMs;
        record('T', "every in %s", s_state);
        act(s_doc["states"][s_state]["every"]);
    }
}

void gamestate_tick() {
    if (!s_ready) return;
    timers();
    heartbeat();
    infoFlush();
}

bool gamestate_set_param(const char* name, float value) {
    int i = name ? findIn(s_params, s_paramCount, name) : -1;
    if (i < 0 || !isfinite(value)) return false;
    Param& p = s_params[i];
    if (!p.ranged) return false;   // a constant: only a new game file changes it
    p.value = constrain(value, p.lo, p.hi);
    char text[40];
    snprintf(text, sizeof(text), "%.3g", p.value);
    record('C', "param %s=%s", p.name, text);
    info("param.%s=%ld", p.name, milli(p.value));
    return true;
}

uint8_t     gamestate_param_count()          { return s_paramCount; }
const char* gamestate_param_name(uint8_t i)  { return i < s_paramCount ? s_params[i].name : ""; }
float       gamestate_param_value(uint8_t i) { return i < s_paramCount ? s_params[i].value : 0.0f; }

bool        gamestate_ready()  { return s_ready; }
const char* gamestate_game()   { return s_game; }
const char* gamestate_state()  { return s_state; }

float gamestate_decay_rate() {
    float v = s_decayParam >= 0 ? s_params[s_decayParam].value : s_decayRate;
    return v > 0.0f ? v : DEFAULT_DECAY_RATE;
}

float gamestate_charge_per_pull() {
    float v = s_chargeParam >= 0 ? s_params[s_chargeParam].value : s_chargePerPull;
    return v > 0.0f ? v : DEFAULT_CHARGE_PER_PULL;
}

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
