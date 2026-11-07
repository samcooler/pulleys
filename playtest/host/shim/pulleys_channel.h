// The palette, as lib/pulleys_channel has it, minus the LEDs.
#pragma once
#include <pulleys_mesh.h>

namespace pulleys {
enum : uint8_t { COLOR_RED = 0, COLOR_BLUE, COLOR_GREEN, COLOR_YELLOW, COLOR_ORANGE, COLOR_PURPLE, COLOR_COUNT };
static constexpr uint8_t COLOR_GAME1_COUNT = 5;
static constexpr uint8_t COLOR_GAME2       = COLOR_PURPLE;
inline const char* color_name(uint8_t c) {
    static const char* const N[COLOR_COUNT] = {"red", "blue", "green", "yellow", "orange", "purple"};
    return c < COLOR_COUNT ? N[c] : "?";
}
uint8_t* palette_table();
inline uint8_t channel_color_index(uint8_t ch) { return palette_table()[ch % CHANNEL_COUNT]; }
inline const uint8_t* palette_get() { return palette_table(); }
}  // namespace pulleys
