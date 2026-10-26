#include "components.h"
#include "gamestate.h"
#include <stdlib.h>
#include <Arduino.h>

static const uint32_t HEARTBEAT_MS = 1000;   // how often the charge is republished

// A pull sets a component to 100% and it drains at the game's default_decay_rate
// (percent per second), so it stays lit 100 / rate seconds.
static uint32_t wearMsNow() {
    return (uint32_t)(100000.0f / gamestate_decay_rate());
}

static const uint8_t MAJOR_COUNT = GAME_MAJOR_COMPONENTS, FINAL_COUNT = GAME_FINAL_COMPONENTS;

// Which rope channel drives which component is the game config's `components`
// section (gamestate_component_for_channel), not decided here.

struct Component {
    bool     isFinal;
    uint32_t litUntil;   // 0 = dark
    uint32_t wearMs;     // how long this pull will keep it lit: its own, so a rate change mid-drain does not jump it
};
static Component s_comp[MAJOR_COUNT + FINAL_COUNT];
static uint8_t s_goalMask = 0; // major component bitset; zero until the first Simon cue

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

static uint8_t litMajorMask(uint32_t now) {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < MAJOR_COUNT; i++)
        if (lit(s_comp[i], now)) mask |= (uint8_t)(1u << i);
    return mask;
}

static bool goalsComplete(uint32_t now) {
    return s_goalMask && litMajorMask(now) == s_goalMask;
}

static uint8_t bitCount(uint8_t n) {
    uint8_t count = 0;
    while (n) { count += n & 1u; n >>= 1; }
    return count;
}

static void randomizeGoals(uint8_t count) {
    uint8_t choices[32];
    uint8_t choiceCount = 0;
    for (uint8_t mask = 1; mask < (1u << MAJOR_COUNT); mask++)
        if (bitCount(mask) == count) choices[choiceCount++] = mask;

    uint8_t filtered[32];
    uint8_t filteredCount = 0;
    for (uint8_t i = 0; i < choiceCount; i++)
        if (choices[i] != s_goalMask) filtered[filteredCount++] = choices[i];
    const uint8_t* pool = filteredCount ? filtered : choices;
    uint8_t poolCount = filteredCount ? filteredCount : choiceCount;
    s_goalMask = pool[(uint8_t)random(poolCount)];
}

static void checkSimonComplete(uint32_t now) {
    if (goalsComplete(now)) gamestate_event(GE_ALL_COMPONENTS_ACTIVATED);
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
        uint32_t wear = s_comp[i].wearMs ? s_comp[i].wearMs : 1;
        // Percent, rounded up so a component that is still lit never reads 0.
        uint32_t pct = left <= 0 ? 0 : ((uint32_t)left * 100UL + wear - 1) / wear;
        charge[i] = pct > 100 ? 100 : (uint8_t)pct;
    }
    uint32_t decay = (uint32_t)(gamestate_decay_rate() * 1000.0f + 0.5f);   // thousandths of a percent per second
    pulleys::mesh_send_components(charge, decay > 65535 ? 65535 : (uint16_t)decay, urgent);
}

void components_init() {
    s_goalMask = 0;
    for (uint8_t i = 0; i < MAJOR_COUNT + FINAL_COUNT; i++) {
        s_comp[i].isFinal  = i >= MAJOR_COUNT;
        s_comp[i].litUntil = 0;
        s_comp[i].wearMs   = 0;
    }
}

bool components_on_cue(const char* cue) {
    if (cue && strcmp(cue, "game_reset_components") == 0) {
        for (Component& component : s_comp) {
            component.litUntil = 0;
            component.wearMs = 0;
        }
        publish(true);
        return false;
    }

    static const char prefix[] = "game_randomize_simon_";
    if (!cue || strncmp(cue, prefix, sizeof(prefix) - 1) != 0) return false;
    const char* digits = cue + sizeof(prefix) - 1;
    char* end = nullptr;
    long count = strtol(digits, &end, 10);
    if (end == digits || strcmp(end, "_goals") != 0 || count < 1 || count > MAJOR_COUNT)
        return false;

    randomizeGoals((uint8_t)count);
    return true;
}

void components_check_simon_complete() {
    checkSimonComplete(millis()); // a retained lit set can already match new goals
}

void components_on_mesh_event(const pulleys::MeshEvent& ev) {
    int idx = gamestate_component_for_channel(ev.channel);
    if (idx < 0) return;

    uint32_t now = millis();
    Component& c = s_comp[idx];
    bool wasLit = lit(c, now);
    c.wearMs   = wearMsNow();
    c.litUntil = now + c.wearMs;
    publish(true);        // lit, or topped back up: the display's bar goes to full
    if (wasLit) return;   // a refill is not a new activation

    if (c.isFinal) {
        gamestate_event(GE_FINAL_COMPONENT_ACTIVATED);
        if (allLit(true, now)) gamestate_event(GE_ALL_FINAL_COMPONENTS_ACTIVATED);
    } else {
        bool isGoal = (s_goalMask & (1u << idx)) != 0;
        if (isGoal) {
            bool noGoalLit = (litMajorMask(now) & s_goalMask) == (uint8_t)(1u << idx);
            if (noGoalLit) gamestate_event(GE_FIRST_GOAL_COMPONENT_ACTIVATED);
            gamestate_event(GE_GOAL_COMPONENT_ACTIVATED);
        } else {
            gamestate_event(GE_NONGOAL_COMPONENT_ACTIVATED);
        }
        checkSimonComplete(now);
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
            checkSimonComplete(now);
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
