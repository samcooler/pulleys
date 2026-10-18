#include "components.h"
#include "gamestate.h"
#include <Arduino.h>

static const uint32_t WEAR_MS = 5000;   // how long a pull keeps a component lit

static const uint8_t MAJOR_COUNT = 5, FINAL_COUNT = 3;

// The rope channel that drives each component: five majors, then three finals.
// Major component 1 is on channel 10.
static const uint8_t CHANNEL_OF[MAJOR_COUNT + FINAL_COUNT] = {
    10, 2, 3, 4, 5,    // major components 1-5
    6, 7, 8,           // final components 1-3
};

struct Component {
    bool     isFinal;
    uint32_t litUntil;   // 0 = dark
};
static Component s_comp[MAJOR_COUNT + FINAL_COUNT];

static bool lit(const Component& c, uint32_t now) {
    return c.litUntil && (int32_t)(c.litUntil - now) > 0;
}

static bool allLit(bool isFinal, uint32_t now) {
    for (const Component& c : s_comp)
        if (c.isFinal == isFinal && !lit(c, now)) return false;
    return true;
}

static bool anyLit(bool isFinal, uint32_t now) {
    for (const Component& c : s_comp)
        if (c.isFinal == isFinal && lit(c, now)) return true;
    return false;
}

void components_init() {
    for (uint8_t i = 0; i < MAJOR_COUNT + FINAL_COUNT; i++) {
        s_comp[i].isFinal  = i >= MAJOR_COUNT;
        s_comp[i].litUntil = 0;
    }
}

void components_on_mesh_event(const pulleys::MeshEvent& ev) {
    int idx = -1;
    for (uint8_t i = 0; i < MAJOR_COUNT + FINAL_COUNT; i++)
        if (CHANNEL_OF[i] == ev.channel) { idx = i; break; }
    if (idx < 0) return;

    uint32_t now = millis();
    Component& c = s_comp[idx];
    bool wasLit = lit(c, now);
    c.litUntil = now + WEAR_MS;
    if (wasLit) return;   // a refill is not a new activation

    if (c.isFinal) {
        gamestate_event(GE_FINAL_COMPONENT_ACTIVATED);
        if (allLit(true, now)) gamestate_event(GE_ALL_FINAL_COMPONENTS_ACTIVATED);
    } else {
        gamestate_event(GE_COMPONENT_ACTIVATED);
        if (allLit(false, now)) gamestate_event(GE_ALL_COMPONENTS_ACTIVATED);
    }
}

void components_tick() {
    uint32_t now = millis();
    for (Component& c : s_comp) {
        // litUntil is cleared once the wear-out has been reported
        if (!c.litUntil || lit(c, now)) continue;
        c.litUntil = 0;
        if (c.isFinal) {
            gamestate_event(GE_FINAL_COMPONENT_DEACTIVATED);
        } else {
            gamestate_event(GE_COMPONENT_DEACTIVATED);
            if (!anyLit(false, now)) gamestate_event(GE_ALL_COMPONENTS_DEACTIVATED);
        }
    }
}
