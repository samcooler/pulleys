#include <cassert>
#include <cstdint>
#include "src/arbiter_mesh/mapping_session.h"
#include "src/arbiter_mesh/calendar_time.h"
#include "src/arbiter_mesh/goal_colors.h"

int main() {
    const uint8_t reordered[] = {4, 2, 0, 3, 1};
    assert(visible_goal_color_mask(true, 0b00101, reordered, 5) == 0b10001);
    assert(visible_goal_color_mask(false, 0b11111, reordered, 5) == 0);
    assert(visible_goal_color_mask(true, 0, reordered, 5) == 0);
    assert(visible_goal_color_mask(true, 0b11111, reordered, 5) == 0b11111);
    const uint8_t unknown[] = {255};
    assert(visible_goal_color_mask(true, 1, unknown, 1) == 0);

    MappingSession s;
    assert(s.open(1, 10, 100));
    assert(!s.open(2, 10, 100));
    assert(!s.open(1, 11, 100));
    s.pull(42, 200);
    s.pull(42, 201);
    assert(s.count == 1 && s.phase == MappingSession::LISTEN);
    for (int i = 0; i < 20; ++i) s.pull(100 + i, 300);
    assert(s.count == 10);
    s.tick(5199); assert(s.phase == MappingSession::LISTEN);
    s.tick(5200); assert(s.phase == MappingSession::PICK);
    s.pull(999, 5300); assert(s.count == 10); // wait for an assignment
    s.assigned = 3;
    s.pull(42, 5400); assert(s.phase == MappingSession::PICK);
    s.pull(999, 5500);
    assert(s.phase == MappingSession::LISTEN && s.count == 1 && s.ids[0] == 999 && s.assigned == -1);
    assert(!s.renew(2, 10, 6000));
    assert(s.renew(1, 10, 6000));
    s.tick(35999); assert(s.active);
    s.tick(36000); assert(!s.active && s.count == 0);
    assert(!s.renew(1, 10, 36001)); // an expired lease cannot revive suppression
    assert(s.open(2, 10, 36002));
    assert(!s.close(1, 10));
    assert(s.close(2, 10));
    assert(s.open(0, 0, 0));
    s.tick(1000000); assert(s.active); // local touchscreen has no lease
    assert(!s.open(2, 10, 1000001));
    assert(s.close(0, 0));
    assert(s.open(3, 10, UINT32_MAX - 100));
    s.tick(29898); assert(s.active);
    s.tick(29899); assert(!s.active); // rollover-safe lease expiry
    assert(valid_calendar_time(2024, 2, 29, 23, 59, 59));
    assert(!valid_calendar_time(2025, 2, 29, 0, 0, 0));
    assert(!valid_calendar_time(2026, 4, 31, 0, 0, 0));
    assert(!valid_calendar_time(2023, 1, 1, 0, 0, 0));
    assert(!valid_calendar_time(2100, 1, 1, 0, 0, 0));
    assert(!valid_calendar_time(2026, 0, 1, 0, 0, 0));
    assert(!valid_calendar_time(2026, 1, 0, 0, 0, 0));
    assert(!valid_calendar_time(2026, 1, 1, 24, 0, 0));
    assert(!valid_calendar_time(2026, 1, 1, 0, -1, 0));
}
