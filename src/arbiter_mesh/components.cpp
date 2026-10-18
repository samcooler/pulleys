#include "components.h"
#include "gamestate.h"
#include <Arduino.h>

static const uint32_t WEAR_MS = 15000;  // how long a pull keeps a component lit
static const uint32_t HEARTBEAT_MS = 1000;   // how often the charge is republished

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

// Tell the displays how charged every component is. A change goes out as a burst;
// the once-a-second repeat covers a display that missed one, and carries the
// drain so it can keep counting down on its own in between.
static void publish(bool urgent) {
    static_assert(MAJOR_COUNT + FINAL_COUNT == pulleys::MESH_COMPONENT_COUNT,
                  "the mesh packet carries one charge per component");
    uint32_t now = millis();
    uint8_t charge[pulleys::MESH_COMPONENT_COUNT];
    for (uint8_t i = 0; i < pulleys::MESH_COMPONENT_COUNT; i++) {
        int32_t left = s_comp[i].litUntil ? (int32_t)(s_comp[i].litUntil - now) : 0;
        charge[i] = left <= 0 ? 0 : (uint8_t)(left >= (int32_t)WEAR_MS ? 255 : left * 255UL / WEAR_MS);
    }
    pulleys::mesh_send_components(charge, (uint16_t)WEAR_MS, urgent);
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
    bool noneLit = !anyLit(c.isFinal, now);   // before this pull lights anything
    c.litUntil = now + WEAR_MS;
    publish(true);        // lit, or topped back up: the display's bar goes to full
    if (wasLit) return;   // a refill is not a new activation

    if (c.isFinal) {
        gamestate_event(GE_FINAL_COMPONENT_ACTIVATED);
        if (allLit(true, now)) gamestate_event(GE_ALL_FINAL_COMPONENTS_ACTIVATED);
    } else {
        // "First" is about the room, not the state: the first major to light
        // while every major was dark. It goes out just before the plain event.
        if (noneLit) gamestate_event(GE_FIRST_COMPONENT_ACTIVATED);
        gamestate_event(GE_COMPONENT_ACTIVATED);
        if (allLit(false, now)) gamestate_event(GE_ALL_COMPONENTS_ACTIVATED);
    }
}

void components_tick() {
    static uint32_t lastPublish = 0;
    uint32_t now = millis();
    bool changed = false;
    for (Component& c : s_comp) {
        // litUntil is cleared once the wear-out has been reported
        if (!c.litUntil || lit(c, now)) continue;
        c.litUntil = 0;
        changed = true;
        if (c.isFinal) {
            gamestate_event(GE_FINAL_COMPONENT_DEACTIVATED);
        } else {
            gamestate_event(GE_COMPONENT_DEACTIVATED);
            if (!anyLit(false, now)) gamestate_event(GE_ALL_COMPONENTS_DEACTIVATED);
        }
    }
    if (changed) {
        publish(true);
        lastPublish = now;
    } else if (now - lastPublish >= HEARTBEAT_MS) {
        publish(false);
        lastPublish = now;
    }
}
