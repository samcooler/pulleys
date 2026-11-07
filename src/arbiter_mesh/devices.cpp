#include "devices.h"
#include "gamestate.h"
#include <pulleys_mesh.h>
#include <pulleys_settings.h>

#define DEV_PREFIX "  [DEVICES]"

static const uint32_t REPEAT_MS = 1000;   // the table goes out at least this often
static const uint8_t  MAX_INPUTS = 32;

struct Setting {
    char       key[48];       // "diorama.lights"
    uint16_t   keyHash;
    JsonArray  values;        // into the adopted document
    uint8_t    want;          // index into values
    uint16_t   isHash;        // last reported value's hash, 0 = never reported
    uint32_t   isAt;
    uint16_t   isFrom;        // reporting device's ID
};
struct Input {
    char     name[48];        // "diorama.door_opened"
    uint16_t hash;
};

static Setting  s_set[pulleys::SETTINGS_MAX];
static uint8_t  s_setCount = 0;
static Input    s_in[MAX_INPUTS];
static uint8_t  s_inCount = 0;
static uint16_t s_epoch   = 0;
static uint16_t s_version = 0;
static bool     s_loaded  = false;
static uint32_t s_lastSend = 0;

// ── Validation ────────────────────────────────────────────────────────────────
static bool isWord(const char* s) {
    if (!s || !*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}

bool devices_validate(JsonDocument& doc, String& why) {
    JsonVariant devs = doc["devices"];
    if (devs.isNull()) return true;
    if (!devs.is<JsonObject>()) { why = "devices: must be a map of device names"; return false; }

    uint16_t seen[pulleys::SETTINGS_MAX + MAX_INPUTS];
    uint8_t  nSeen = 0, nSet = 0, nIn = 0;
    auto unique = [&](uint16_t h, const String& name) {
        for (uint8_t i = 0; i < nSeen; i++)
            if (seen[i] == h) { why = "devices: '" + name + "' collides with another name on the air; rename one"; return false; }
        seen[nSeen++] = h;
        return true;
    };

    for (JsonPair d : devs.as<JsonObject>()) {
        String dev = d.key().c_str();
        if (!isWord(dev.c_str())) { why = "devices." + dev + ": a device name is lower-case letters, digits and _"; return false; }
        JsonObject body = d.value().as<JsonObject>();
        if (body.isNull()) { why = "devices." + dev + ": must be a map with set and/or emits"; return false; }
        for (JsonPair kv : body) {
            if (strcmp(kv.key().c_str(), "set") && strcmp(kv.key().c_str(), "emits")) {
                why = "devices." + dev + ": unknown key '" + kv.key().c_str() + "'; use set, emits";
                return false;
            }
        }
        for (JsonPair p : body["set"].as<JsonObject>()) {
            String key = dev + "." + p.key().c_str();
            if (!isWord(p.key().c_str())) { why = "devices." + key + ": a setting name is lower-case letters, digits and _"; return false; }
            JsonArray values = p.value().as<JsonArray>();
            if (values.isNull() || values.size() == 0) { why = "devices." + key + ": list the values it may take"; return false; }
            uint16_t vh[16]; uint8_t nv = 0;
            for (JsonVariant v : values) {
                if (!v.is<const char*>() || !isWord(v.as<const char*>())) {
                    why = "devices." + key + ": values are words (quote on/off/yes/no: YAML reads them as true/false)";
                    return false;
                }
                if (nv >= 16) { why = "devices." + key + ": at most 16 values"; return false; }
                uint16_t h = pulleys::settings_hash(v.as<const char*>());
                for (uint8_t i = 0; i < nv; i++)
                    if (vh[i] == h) { why = "devices." + key + ": two values are the same on the air; rename one"; return false; }
                vh[nv++] = h;
            }
            if (++nSet > pulleys::SETTINGS_MAX) { why = String("devices: at most ") + pulleys::SETTINGS_MAX + " settings"; return false; }
            if (key.length() >= sizeof(Setting::key)) { why = "devices." + key + ": name too long"; return false; }
            if (!unique(pulleys::settings_hash(key.c_str()), key)) return false;
        }
        JsonVariant emits = body["emits"];
        if (!emits.isNull() && !emits.is<JsonArray>()) { why = "devices." + dev + ".emits: must be a list"; return false; }
        for (JsonVariant e : emits.as<JsonArray>()) {
            if (!e.is<const char*>() || !isWord(e.as<const char*>())) {
                why = "devices." + dev + ".emits: inputs are lower-case words"; return false;
            }
            String name = dev + "." + e.as<const char*>();
            if (++nIn > MAX_INPUTS) { why = String("devices: at most ") + MAX_INPUTS + " inputs"; return false; }
            if (name.length() >= sizeof(Input::name)) { why = "devices." + name + ": name too long"; return false; }
            if (!unique(pulleys::settings_hash(name.c_str()), name)) return false;
        }
    }
    return true;
}

// "diorama.lights" -> the declared list of values, or null.
static JsonArray valuesOf(JsonDocument& doc, const char* key) {
    const char* dot = strchr(key, '.');
    if (!dot) return JsonArray();
    String dev(key, dot - key);
    return doc["devices"][dev]["set"][dot + 1].as<JsonArray>();
}

bool devices_check_set(JsonDocument& doc, JsonVariant set, const String& where, String& why) {
    if (set.isNull()) return true;
    if (!set.is<JsonObject>()) { why = where + ".set: must be a map of device.setting: value"; return false; }
    for (JsonPair kv : set.as<JsonObject>()) {
        JsonArray values = valuesOf(doc, kv.key().c_str());
        if (values.isNull()) {
            why = where + ".set: '" + kv.key().c_str() + "' is not a setting under devices";
            return false;
        }
        bool ok = false;
        const char* v = kv.value().as<const char*>();
        for (JsonVariant allowed : values) if (v && strcmp(v, allowed.as<const char*>()) == 0) ok = true;
        if (!ok) {
            why = where + ".set: " + kv.key().c_str() + " cannot be '" + (v ? v : "?") +
                  "' (quote on/off/yes/no: YAML reads them as true/false)";
            return false;
        }
    }
    return true;
}

bool devices_is_input(JsonDocument& doc, const char* name) {
    const char* dot = strchr(name, '.');
    if (!dot) return false;
    String dev(name, dot - name);
    for (JsonVariant e : doc["devices"][dev]["emits"].as<JsonArray>())
        if (strcmp(e.as<const char*>(), dot + 1) == 0) return true;
    return false;
}

// ── The table ─────────────────────────────────────────────────────────────────
static void publish(bool urgent) {
    pulleys::SettingEntry e[pulleys::SETTINGS_MAX];
    for (uint8_t i = 0; i < s_setCount; i++) {
        e[i].key   = s_set[i].keyHash;
        e[i].value = pulleys::settings_hash(s_set[i].values[s_set[i].want].as<const char*>());
    }
    pulleys::settings_send(s_epoch, s_version, e, s_setCount, urgent);
    s_lastSend = millis();
}

static void onDevice(const pulleys::MeshFlood& f, bool /*relayed*/) {
    pulleys::DeviceMsg m;
    if (!pulleys::settings_decode_device(f, m)) return;
    if (m.kind == pulleys::DEVICE_INPUT && m.count >= 1) {
        for (uint8_t i = 0; i < s_inCount; i++) {
            if (s_in[i].hash != m.entry[0].key) continue;
            Serial.printf(DEV_PREFIX " input %s from 0x%04X\n", s_in[i].name, f.h.originId);
            gamestate_input(s_in[i].name);
            return;
        }
        Serial.printf(DEV_PREFIX " input %04X from 0x%04X: not in this game\n", m.entry[0].key, f.h.originId);
    } else if (m.kind == pulleys::DEVICE_REPORT) {
        for (uint8_t r = 0; r < m.count; r++) {
            for (uint8_t i = 0; i < s_setCount; i++) {
                if (s_set[i].keyHash != m.entry[r].key) continue;
                s_set[i].isHash = m.entry[r].value;
                s_set[i].isAt   = millis();
                s_set[i].isFrom = f.h.originId;
            }
        }
    }
}

void devices_init() {
    s_epoch = (uint16_t)esp_random();
    pulleys::mesh_on_flood(pulleys::MESH_MSG_DEVICE, onDevice);
}

void devices_load(JsonDocument& doc) {
    s_setCount = s_inCount = 0;
    for (JsonPair d : doc["devices"].as<JsonObject>()) {
        for (JsonPair p : d.value()["set"].as<JsonObject>()) {
            if (s_setCount >= pulleys::SETTINGS_MAX) break;
            Setting& s = s_set[s_setCount++];
            snprintf(s.key, sizeof(s.key), "%s.%s", d.key().c_str(), p.key().c_str());
            s.keyHash = pulleys::settings_hash(s.key);
            s.values  = p.value().as<JsonArray>();
            s.want    = 0;
            s.isHash  = 0;
            s.isAt    = 0;
            s.isFrom  = 0;
        }
        for (JsonVariant e : d.value()["emits"].as<JsonArray>()) {
            if (s_inCount >= MAX_INPUTS) break;
            Input& in = s_in[s_inCount++];
            snprintf(in.name, sizeof(in.name), "%s.%s", d.key().c_str(), e.as<const char*>());
            in.hash = pulleys::settings_hash(in.name);
        }
    }
    s_loaded = true;
    s_version++;
    Serial.printf(DEV_PREFIX " %u settings, %u inputs\n", s_setCount, s_inCount);
    publish(true);
}

void devices_set(const char* key, const char* value) {
    for (uint8_t i = 0; i < s_setCount; i++) {
        Setting& s = s_set[i];
        if (strcmp(s.key, key) != 0) continue;
        for (uint8_t v = 0; v < s.values.size(); v++) {
            if (strcmp(s.values[v].as<const char*>(), value) != 0) continue;
            if (s.want == v) return;
            s.want = v;
            s_version++;
            publish(true);
            return;
        }
    }
}

void devices_tick() {
    if (s_loaded && millis() - s_lastSend >= REPEAT_MS) publish(false);
}

// The name of a reported value, or "?" if it is not one this setting may take.
static const char* nameOfValue(const Setting& s, uint16_t h) {
    for (JsonVariant v : s.values)
        if (pulleys::settings_hash(v.as<const char*>()) == h) return v.as<const char*>();
    return "?";
}

void devices_print() {
    if (!s_setCount) { Serial.println(DEV_PREFIX " none in this game"); return; }
    for (uint8_t i = 0; i < s_setCount; i++) {
        const Setting& s = s_set[i];
        const char* want = s.values[s.want].as<const char*>();
        if (!s.isAt) {
            Serial.printf(DEV_PREFIX " %-22s want=%-10s  never reported\n", s.key, want);
            continue;
        }
        const char* is = nameOfValue(s, s.isHash);
        Serial.printf(DEV_PREFIX " %-22s want=%-10s is=%-10s %s %lus ago from 0x%04X\n",
                      s.key, want, is, strcmp(want, is) ? "!!" : "ok",
                      (unsigned long)((millis() - s.isAt) / 1000), s.isFrom);
    }
}

String devices_json() {
    String out = "{";
    for (uint8_t i = 0; i < s_setCount; i++) {
        const Setting& s = s_set[i];
        if (i) out += ",";
        out += "\"" + String(s.key) + "\":{\"want\":\"" + s.values[s.want].as<const char*>() + "\"";
        if (s.isAt)
            out += ",\"is\":\"" + String(nameOfValue(s, s.isHash)) + "\",\"ageS\":" +
                   String((millis() - s.isAt) / 1000);
        out += "}";
    }
    return out + "}";
}
