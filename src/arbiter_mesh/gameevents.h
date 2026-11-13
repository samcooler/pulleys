#pragma once

#include <stdint.h>
#include <string.h>

// ── Game events — the vocabulary between the mapper and the state machine ────
//
// components.cpp turns what the sensors do into these; gamestate.cpp reacts to
// them. Neither knows how the other works: this list, and gamestate_event(),
// is the whole interface. The names are the keys a state lists under `on:` in
// the game config (GAME_FORMAT.md). Keep in step with KNOWN_EVENTS in
// playtest/spec.py, which the validator and the playtest engine use. A game
// also hears <counter>_reached and its devices' inputs, by name.

// Five major components, one per game1 colour. Keep in step with
// COMPONENT_COUNTS in tools/game_to_json.py. The finals are not counted here:
// every purple channel is one, so there are as many as the palette says.
// The component state display shows the majors and GAME_FINAL_BARS finals
// (pulleys::MESH_COMPONENT_COUNT bars in all).
static const uint8_t GAME_MAJOR_COMPONENTS = 5;
static const uint8_t GAME_FINAL_BARS       = 3;

enum GameEvent : uint8_t {
    GE_FIRST_GOAL_COMPONENT_ACTIVATED,
    GE_GOAL_COMPONENT_ACTIVATED,
    GE_NONGOAL_COMPONENT_ACTIVATED,
    GE_COMPONENT_DEACTIVATED,
    GE_ALL_COMPONENTS_ACTIVATED,
    GE_ALL_COMPONENTS_DEACTIVATED,
    GE_FINAL_COMPONENT_ACTIVATED,
    GE_FINAL_COMPONENT_DEACTIVATED,
    GE_ALL_FINAL_COMPONENTS_ACTIVATED,
    // One for every pull, before its charge goes in: `pull` for any rope,
    // then which kind of rope it was (a goal colour, another major, purple).
    GE_PULL,
    GE_GOAL_PULL,
    GE_NONGOAL_PULL,
    GE_FINAL_PULL,
    // A pull of a major's rope that is not a goal, by its colour, after
    // GE_NONGOAL_PULL. In colour order, so GE_NONGOAL_RED_PULL + colour index.
    GE_NONGOAL_RED_PULL,
    GE_NONGOAL_BLUE_PULL,
    GE_NONGOAL_GREEN_PULL,
    GE_NONGOAL_YELLOW_PULL,
    GE_NONGOAL_ORANGE_PULL,
    GE_COUNT
};

inline bool gameevent_is_pull(GameEvent e) { return e >= GE_PULL && e <= GE_NONGOAL_ORANGE_PULL; }

inline const char* gameevent_name(GameEvent e) {
    static const char* const names[GE_COUNT] = {
        "first_goal_component_activated",
        "goal_component_activated",
        "nongoal_component_activated",
        "component_deactivated",
        "all_components_activated",
        "all_components_deactivated",
        "final_component_activated",
        "final_component_deactivated",
        "all_final_components_activated",
        "pull",
        "goal_pull",
        "nongoal_pull",
        "final_pull",
        "nongoal_red_pull",
        "nongoal_blue_pull",
        "nongoal_green_pull",
        "nongoal_yellow_pull",
        "nongoal_orange_pull",
    };
    return e < GE_COUNT ? names[e] : "?";
}

// Name -> event, for events injected by hand. False if it is not one of ours.
inline bool gameevent_from_name(const char* name, GameEvent& out) {
    for (uint8_t i = 0; i < GE_COUNT; i++) {
        if (strcmp(name, gameevent_name((GameEvent)i)) == 0) { out = (GameEvent)i; return true; }
    }
    return false;
}
