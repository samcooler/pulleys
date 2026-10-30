#include "components.h"
#include "gamestate.h"
#include "installcfg.h"
#include <stdlib.h>
#include <Arduino.h>
#include <pulleys_channel.h>

static const uint32_t HEARTBEAT_MS = 1000;   // how often the charge is republished

// A pull sets a component to 100% and it drains at the game's default_decay_rate
// (percent per second), so it stays lit 100 / rate seconds.
static uint32_t wearMsNow() {
    return (uint32_t)(100000.0f / gamestate_decay_rate());
}

static const uint8_t MAJOR_COUNT = GAME_MAJOR_COMPONENTS;

// Which rope drives what follows the palette, not a fixed table:
//
//   majors  the five game1 colours, in the order the config's `components`
//           lists them (gamestate_major_for_channel). Every rope wearing a
//           colour today feeds that colour's component.
//   finals  every channel wearing game2's purple is a final of its own. Game2
//           is won when all of them are lit at once -- all the purple channels
//           that have a rope on them, since a channel nobody can pull would
//           make it unwinnable. Painting a channel purple on the COLOURS
//           screen is what makes it a final.

struct Component {
    uint32_t litUntil;   // 0 = dark
    uint32_t wearMs;     // how long this pull will keep it lit: its own, so a rate change mid-drain does not jump it
};
static Component s_major[MAJOR_COUNT];
static Component s_final[pulleys::CHANNEL_COUNT];   // by channel; only purple ones are used
static uint8_t s_goalMask = 0; // major component bitset; zero until the first Simon cue

static bool lit(const Component& c, uint32_t now) {
    return c.litUntil && (int32_t)(c.litUntil - now) > 0;
}

static bool isFinalChannel(uint8_t ch) {
    return pulleys::channel_color_index(ch) == pulleys::COLOR_GAME2;
}

// A final that counts toward the win: purple, with a rope mapped to it.
static bool isRopedFinal(uint8_t ch) {
    if (!isFinalChannel(ch)) return false;
    const pulleys::MeshConfigRow* rows = installcfg_rows();
    for (uint8_t i = 0; i < installcfg_row_count(); i++)
        if (rows[i].channel == ch) return true;
    return false;
}

static bool allFinalsLit(uint32_t now) {
    uint8_t finals = 0;
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT; ch++) {
        if (!isRopedFinal(ch)) continue;
        if (!lit(s_final[ch], now)) return false;
        finals++;
    }
    return finals > 0;
}

static bool anyMajorLit(uint32_t now) {
    for (const Component& c : s_major) if (lit(c, now)) return true;
    return false;
}

static uint8_t litMajorMask(uint32_t now) {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < MAJOR_COUNT; i++)
        if (lit(s_major[i], now)) mask |= (uint8_t)(1u << i);
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
// Percent, rounded up so a component that is still lit never reads 0.
static uint8_t chargeOf(const Component& c, uint32_t now) {
    int32_t left = c.litUntil ? (int32_t)(c.litUntil - now) : 0;
    uint32_t wear = c.wearMs ? c.wearMs : 1;
    uint32_t pct = left <= 0 ? 0 : ((uint32_t)left * 100UL + wear - 1) / wear;
    return pct > 100 ? 100 : (uint8_t)pct;
}

// The display has a bar per major and GAME_FINAL_BARS for the finals, which go
// to the lowest-numbered roped purple channels. More purple channels than that
// still count toward the win; they just have no bar. A bar with no channel
// behind it goes out with MESH_COLOR_UNKNOWN.
static void publish(bool urgent) {
    static_assert(MAJOR_COUNT + GAME_FINAL_BARS == pulleys::MESH_COMPONENT_COUNT,
                  "the mesh packet carries one charge per bar");
    uint32_t now = millis();
    uint8_t charge[pulleys::MESH_COMPONENT_COUNT] = {};
    uint8_t color[pulleys::MESH_COMPONENT_COUNT];
    memset(color, pulleys::MESH_COLOR_UNKNOWN, sizeof(color));
    for (uint8_t i = 0; i < MAJOR_COUNT; i++) {
        charge[i] = chargeOf(s_major[i], now);
        color[i]  = gamestate_major_color(i);
    }
    uint8_t bar = MAJOR_COUNT;
    for (uint8_t ch = 0; ch < pulleys::CHANNEL_COUNT && bar < pulleys::MESH_COMPONENT_COUNT; ch++) {
        if (!isRopedFinal(ch)) continue;
        charge[bar] = chargeOf(s_final[ch], now);
        color[bar]  = pulleys::COLOR_GAME2;
        bar++;
    }
    uint32_t decay = (uint32_t)(gamestate_decay_rate() * 1000.0f + 0.5f);   // thousandths of a percent per second
    pulleys::mesh_send_components(charge, color, decay > 65535 ? 65535 : (uint16_t)decay, urgent);
}

static void resetAll() {
    memset(s_major, 0, sizeof(s_major));
    memset(s_final, 0, sizeof(s_final));
}

void components_init() {
    s_goalMask = 0;
    resetAll();
}

bool components_on_cue(const char* cue) {
    if (cue && strcmp(cue, "game_reset_components") == 0) {
        resetAll();
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

// Light (or top up) a component. True if it was dark: a refill is not a new
// activation.
static bool light(Component& c, uint32_t now) {
    bool wasLit = lit(c, now);
    c.wearMs   = wearMsNow();
    c.litUntil = now + c.wearMs;
    publish(true);        // lit, or topped back up: the display's bar goes to full
    return !wasLit;
}

void components_on_mesh_event(const pulleys::MeshEvent& ev) {
    if (!gamestate_ready() || ev.channel >= pulleys::CHANNEL_COUNT) return;
    uint32_t now = millis();

    if (isFinalChannel(ev.channel)) {
        if (!light(s_final[ev.channel], now)) return;
        gamestate_event(GE_FINAL_COMPONENT_ACTIVATED);
        if (allFinalsLit(now)) gamestate_event(GE_ALL_FINAL_COMPONENTS_ACTIVATED);
        return;
    }

    int idx = gamestate_major_for_channel(ev.channel);
    if (idx < 0) return;
    if (!light(s_major[idx], now)) return;
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

void components_tick() {
    static uint32_t lastPublish = 0;
    uint32_t now = millis();
    bool changed = false;
    // litUntil is cleared once the wear-out has been reported
    for (Component& c : s_major) {
        if (!c.litUntil || lit(c, now)) continue;
        c.litUntil = 0;
        changed = true;
        gamestate_event(GE_COMPONENT_DEACTIVATED);
        if (!anyMajorLit(now)) gamestate_event(GE_ALL_COMPONENTS_DEACTIVATED);
        checkSimonComplete(now);
    }
    for (Component& c : s_final) {
        if (!c.litUntil || lit(c, now)) continue;
        c.litUntil = 0;
        changed = true;
        gamestate_event(GE_FINAL_COMPONENT_DEACTIVATED);
    }
    if (changed) {
        publish(true);
        lastPublish = now;
    } else if (now - lastPublish >= HEARTBEAT_MS) {
        publish(false);
        lastPublish = now;
    }
}
