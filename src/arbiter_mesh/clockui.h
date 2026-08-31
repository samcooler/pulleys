#pragma once

#include <pulleys_rtc.h>

// A touch panel to set the RTC. Hidden until the corner button is tapped, so it
// never covers the monitor during an event; open it, dial the time in, Save
// writes the RTC and marks the clock source trusted.
void clockui_init(pulleys::RTC* rtc);
void clockui_tick();     // refresh the live-time readout while open
bool clockui_is_open();
