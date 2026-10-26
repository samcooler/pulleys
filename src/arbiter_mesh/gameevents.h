#pragma once

#include <stdint.h>
#include <string.h>

// ── Game events — the vocabulary between the mapper and the state machine ────
//
// components.cpp turns what the sensors do into these; gamestate.cpp reacts to
// them. Neither knows how the other works: this list, and gamestate_event(),
// is the whole interface. The names are the keys a state lists under `on:` in
// the game config (GAME_FORMAT.md).

// How many components there are: major ones, then final ones. The component
// state display has a bar for each (pulleys::MESH_COMPONENT_COUNT). Keep in step
// with COMPONENT_COUNTS in tools/game_to_json.py.
static const uint8_t GAME_MAJOR_COMPONENTS = 5;
static const uint8_t GAME_FINAL_COMPONENTS = 3;

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
    GE_COUNT
};

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
