#pragma once

#include <stdint.h>
#include <pulleys_rtc.h>

// ── Where the clock's time came from ─────────────────────────────────────────
//
// There is no coin cell on this board, so the RTC loses its time whenever power
// drops. That makes the provenance of a timestamp as important as the timestamp
// itself: a row stamped from a stale firmware build date looks exactly like a
// real one, and would quietly corrupt the study.
//
// So every row records its source, and the clock is re-seeded from the most
// recent evidence available rather than from whatever is most convenient:
//
//   NONE      no RTC, or never set — rows carry uptime only
//   BUILD     firmware build time; right if flashed at the event, stale if not
//   RESTORED  last-known time persisted to NVS before a power cut. Misses the
//             length of the outage, so it runs slow — but by the outage, not
//             by days
//   SET       set explicitly over serial. The only fully trusted source.

enum TimeSource : uint8_t {
    TIME_NONE = 0,
    TIME_BUILD,
    TIME_RESTORED,
    TIME_SET,
};

void        clocksrc_init(pulleys::RTC* rtc);
void        clocksrc_tick();          // persists a snapshot roughly once a minute
void        clocksrc_mark_set();      // call after an explicit serial set
TimeSource  clocksrc_source();
const char* clocksrc_source_str();
