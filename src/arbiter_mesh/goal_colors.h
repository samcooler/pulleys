#pragma once
#include <stdint.h>

// Game goals address components, whose configured order need not be palette
// order. Both UIs show named palette colours (wire indices 0–5), only in states
// where goals apply. Unknown colour mappings must never become goal bits.
inline uint8_t visible_goal_color_mask(bool visible, uint8_t componentGoals,
                                      const uint8_t* majorColors, uint8_t majorCount) {
    if (!visible) return 0;
    uint8_t colors = 0;
    for (uint8_t i = 0; i < majorCount && i < 8; ++i)
        if ((componentGoals & (1u << i)) && majorColors[i] < 6)
            colors |= 1u << majorColors[i];
    return colors;
}
