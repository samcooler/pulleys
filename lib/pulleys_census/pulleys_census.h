#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <pulleys_mesh.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>

// ── pulleys_census — asking the whole piece what it is ───────────────────────
//
// `./flash_all.sh -l` answers "what is plugged in", which stopped being the
// same question as "what is out there" the moment boards could be reflashed
// without a cable. A rope twelve feet up is part of the piece whether or not
// anything is plugged into it, and after an over-the-air round the thing you
// most want to know is which boards actually took the new image.
//
// So: one board broadcasts HELLO, every board answers with a CENSUS naming
// itself — role, env, build stamp, a role-specific detail, uptime. The asker
// prints one line per answer, in the same one-line key=value style as
// PULLEYS-ID, and tools/survey.py tabulates them.
//
// This is deliberately not part of OTA. It runs over the plain mesh, with no
// AP and no interruption: every board answers from inside its normal loop and
// carries on with the art. Surveying the piece should never disturb it.
//
// Replies are spread across a window the asker names. A dozen boards answering
// in the same millisecond is a dozen boards colliding, and the one you would
// lose is the one furthest away — which is the one you most wanted to hear
// from.
//
// Usage:
//   pulleys::census_init();                    // in setup(), after mesh_init()
//   pulleys::census_set_detail("ch5 lin");     // whenever the role knows more
//   pulleys::census_poll();                    // every loop(), beside mesh_poll()
//   pulleys::census_request();                 // "CENSUS" on serial

namespace pulleys {

#define PULLEYS_CENSUS_PREFIX "PULLEYS-CENSUS"

// Long enough that a dozen boards rarely collide, short enough that a survey
// feels immediate. The asker listens a little past this before giving up.
static constexpr uint16_t CENSUS_SPREAD_MS = 900;

namespace _census {

struct State {
    char     detail[16] = {};
    uint16_t pendingNonce = 0;
    uint32_t replyAt = 0;      // 0 = nothing owed
    bool     asking  = false;
};

inline State& S() { static State s; return s; }

inline void onHello(const MeshHello& h) {
    State& s = S();
    // Answer at a random point inside the asker's window rather than at once.
    uint16_t spread = h.spreadMs ? h.spreadMs : CENSUS_SPREAD_MS;
    s.pendingNonce = h.nonce;
    s.replyAt = millis() + (uint32_t)random(0, spread);
}

inline const char* roleName(uint8_t originType) {
    switch (originType) {
        case MESH_ORIGIN_SENSOR:  return "SENSOR";
        case MESH_ORIGIN_SCREEN:  return "SCREEN";
        case MESH_ORIGIN_ARBITER: return "ARBITER";
        default:                  return "UNKNOWN";
    }
}

// One line per board, same rules as PULLEYS-ID: single line, space-separated
// key=value, parse by key and never by position. `detail` is the one field
// that may be empty, and it is quoted so an empty one still parses.
inline void print(uint16_t id, uint8_t originType, const char* env,
                  const char* build, const char* detail, uint32_t uptime,
                  bool self) {
    Serial.printf(PULLEYS_CENSUS_PREFIX " id=%04X name=N-%04X role=%s env=%s "
                  "build=%s detail=\"%s\" up=%lu self=%d\n",
                  id, id, roleName(originType),
                  (env && env[0]) ? env : "?",
                  (build && build[0]) ? build : "?",
                  detail ? detail : "",
                  (unsigned long)uptime, self ? 1 : 0);
}

inline void onCensus(const MeshCensus& c) {
    // Copy through fixed buffers: nothing on the wire is promised to be
    // terminated, and a run-on string here would take the whole line with it.
    char env[17] = {}, build[25] = {}, detail[17] = {};
    memcpy(env, c.env, sizeof(c.env));
    memcpy(build, c.build, sizeof(c.build));
    memcpy(detail, c.detail, sizeof(c.detail));
    print(c.originId, c.originType, env, build, detail, c.uptimeSecs, false);
}

}  // namespace _census

// What this role wants shown beside its name in a survey: a sensor's rope and
// mode, a screen's display. Free-form on purpose — the useful fact differs per
// role, and a survey is for reading, not for parsing into a schema.
inline void census_set_detail(const char* detail) {
    strncpy(_census::S().detail, detail ? detail : "",
            sizeof(_census::S().detail) - 1);
}

inline void census_init() {
    mesh_on_hello(_census::onHello);
    mesh_on_census(_census::onCensus);
}

inline void census_poll() {
    _census::State& s = _census::S();
    if (!s.replyAt || (int32_t)(millis() - s.replyAt) < 0) return;
    s.replyAt = 0;
    mesh_send_census(s.pendingNonce, PULLEYS_ENV, whoami_build(),
                     s.detail, millis() / 1000);
}

// Ask everyone. Answers arrive over the next spread window and print as they
// land; this board reports itself immediately, since it will not hear its own
// broadcast.
inline void census_request(uint16_t spreadMs = CENSUS_SPREAD_MS) {
    Serial.printf(PULLEYS_CENSUS_PREFIX " begin spread=%ums\n", spreadMs);
    _census::print(identity_id(), _mesh::S().originType, PULLEYS_ENV,
                   whoami_build(), _census::S().detail, millis() / 1000, true);
    mesh_send_hello(spreadMs);
}

// Serial hook, for roles that already read line-oriented commands.
inline bool census_handle_serial(const char* line) {
    if (!line || strncmp(line, "CENSUS", 6) != 0) return false;
    uint16_t spread = CENSUS_SPREAD_MS;
    int v = atoi(line + 6);
    if (v > 0 && v <= 10000) spread = (uint16_t)v;
    census_request(spread);
    return true;
}

}  // namespace pulleys
