#include "components.h"
#include "gamestate.h"
#include "installcfg.h"
#include <stdlib.h>
#include <Arduino.h>
#include <pulleys_channel.h>

static const uint32_t HEARTBEAT_MS = 1000;   // how often the charge is republished

// A pull adds the game's charge_per_pull and the charge drains at its
// default_decay_rate (both percent), so no one pull fills a component: it takes
// pulls coming faster than the drain. Reaching 100% lights it, and it stays lit
// until it has drained all the way back to 0 -- the hysteresis keeps a
// component hovering near full from flickering on and off.
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
    float    charge;     // percent, as of stampMs
    uint32_t stampMs;
    bool     on;         // reached 100% and has not yet drained to 0
};
static Component s_major[MAJOR_COUNT];
static Component s_final[pulleys::CHANNEL_COUNT];   // by channel; only purple ones are used
static uint8_t s_goalMask = 0; // major component bitset; zero until the first Simon cue

static void publish(bool urgent);

// Bring the charge up to `now`. Drains at whatever the rate is today, so a game
// posted with a new rate takes over from the charge as it stands.
static void settle(Component& c, uint32_t now) {
    if (c.charge > 0.0f) {
        c.charge -= gamestate_decay_rate() * (float)(now - c.stampMs) / 1000.0f;
        if (c.charge < 0.0f) c.charge = 0.0f;
    }
    c.stampMs = now;
}

// Whether it counts as activated. components_tick() turns it off once it has
// drained, so this is the flag and not a reading of the charge.
static bool lit(const Component& c, uint32_t /*now*/) {
    return c.on;
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
    gamestate_note_goals(s_goalMask);
    publish(true);   // the extractors' outlines move with the goals
}

void components_deal_goals(uint8_t count) {
    if (count < 1) count = 1;
    if (count > MAJOR_COUNT) count = MAJOR_COUNT;
    randomizeGoals(count);
}

uint8_t components_goal_mask() { return s_goalMask; }

static void checkSimonComplete(uint32_t now) {
    if (goalsComplete(now)) gamestate_event(GE_ALL_COMPONENTS_ACTIVATED);
}

// Tell the displays how charged every component is. A change goes out as a burst;
// the once-a-second repeat covers a display that missed one, and carries the
// drain so it can keep counting down on its own in between.
// Percent, rounded up so a component that is still lit never reads 0.
static uint8_t chargeOf(Component& c, uint32_t now) {
    settle(c, now);
    float pct = ceilf(c.charge);
    return pct > 100.0f ? 100 : (uint8_t)pct;
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
    uint8_t goals = gamestate_goals_visible() ? s_goalMask : 0;
    pulleys::mesh_send_components(charge, color, decay > 65535 ? 65535 : (uint16_t)decay, urgent, goals);
}

void components_publish() { publish(true); }

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

// Pour one pull into a component. True if that filled it and it was dark: a
// top-up of one already lit is not a new activation.
static bool pull(Component& c, uint32_t now) {
    settle(c, now);
    c.charge += gamestate_charge_per_pull();
    if (c.charge > 100.0f) c.charge = 100.0f;
    bool lights = !c.on && c.charge >= 100.0f;
    if (lights) c.on = true;
    publish(true);        // the display's bar rises now, not at the next heartbeat
    return lights;
}

void components_on_mesh_event(const pulleys::MeshEvent& ev) {
    if (!gamestate_ready() || ev.channel >= pulleys::CHANNEL_COUNT) return;

    // The pull itself first, so a state can react before the charge goes in:
    // a pull that starts a round lands its charge in the new round, after the
    // round's reset rather than before it.
    if (isFinalChannel(ev.channel)) {
        gamestate_event(GE_PULL);
        gamestate_event(GE_FINAL_PULL);
    } else {
        int idx = gamestate_major_for_channel(ev.channel);
        if (idx < 0) return;
        bool goal = (s_goalMask & (1u << idx)) != 0;
        gamestate_event(GE_PULL);
        gamestate_event(goal ? GE_GOAL_PULL : GE_NONGOAL_PULL);
    }

    // Then the charge, where things stand now: a reaction may have recoloured
    // the ropes or dealt new goals.
    uint32_t now = millis();

    if (isFinalChannel(ev.channel)) {
        if (!pull(s_final[ev.channel], now)) return;
        gamestate_event(GE_FINAL_COMPONENT_ACTIVATED);
        if (allFinalsLit(now)) gamestate_event(GE_ALL_FINAL_COMPONENTS_ACTIVATED);
        return;
    }

    int idx = gamestate_major_for_channel(ev.channel);
    if (idx < 0) return;
    if (!pull(s_major[idx], now)) return;
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
    // A lit component goes dark once it has drained to empty.
    for (Component& c : s_major) {
        if (!c.on) continue;
        settle(c, now);
        if (c.charge > 0.0f) continue;
        c.on = false;
        changed = true;
        gamestate_event(GE_COMPONENT_DEACTIVATED);
        if (!anyMajorLit(now)) gamestate_event(GE_ALL_COMPONENTS_DEACTIVATED);
        checkSimonComplete(now);
    }
    for (Component& c : s_final) {
        if (!c.on) continue;
        settle(c, now);
        if (c.charge > 0.0f) continue;
        c.on = false;
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
