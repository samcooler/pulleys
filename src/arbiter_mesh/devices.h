#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// ── Devices — the game's settings for every device, and what comes back ──────
//
// The game file declares its devices: the settings each one takes, with the
// values each may hold (the first is where it starts), and the inputs it can
// emit. A state sets values with `set: { diorama.lights: creepy }`; this file
// keeps the resulting table and broadcasts it (pulleys_settings) on every
// change and once a second. A device's input arrives here and goes to the game
// as an event named "device.input"; a device's report of what it is actually
// doing is kept beside what it was told, for the console and /state.
//
//   devices:
//     diorama:
//       set:
//         lights: [dark, creepy]       # dark is where it starts
//         latch:  [locked, open]
//       emits: [door_opened, door_closed]

// Checks for gamestate's validate(), on a document not yet adopted.
bool devices_validate(JsonDocument& doc, String& why);
bool devices_check_set(JsonDocument& doc, JsonVariant set, const String& where, String& why);
bool devices_is_input(JsonDocument& doc, const char* name);   // "device.input", declared

void devices_init();                       // once, after mesh_init
void devices_load(JsonDocument& doc);      // the adopted game: every setting at its first value
void devices_set(const char* key, const char* value);   // validated on load
void devices_tick();                       // every loop: the once-a-second repeat
// What the game wants a setting to be, or nullptr if this game has no such
// setting. For a device that is the arbiter itself (the queue sign).
const char* devices_want(const char* key);
// The arbiter's own device saying what it is doing, as a mesh device's
// report would: so the panel and /state see it as fact, not as silent.
void devices_report_local(const char* key, const char* value);
void devices_print();                      // console: wanted vs reported, per setting
String devices_json();                     // {"diorama.lights":{"want":"creepy","is":"creepy","ageS":3}, ...}
// One line for the panel: "devices: 3/3 as set", or the first that is not
// (told one thing, reporting another; or not heard from). Empty with no
// devices. True if all is well.
bool devices_summary(char* out, size_t n);
