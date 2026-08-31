#pragma once

#include <stdint.h>
#include <pulleys_mesh.h>
#include <pulleys_rtc.h>

// ── Event log — one CSV row per detection, on the microSD ────────────────────
//
// Raw rows, not summaries: bout lengths, active-channel counts and time-of-day
// histograms are all recoverable afterwards, and keeping the detail means the
// study can ask questions nobody thought of during the event. At this scale
// size is irrelevant — 50k detections is a couple of MB on a 30 GB card.
//
// Rows are queued and written from the main loop rather than from the mesh
// callback, so a slow SD write cannot stall the display.

bool eventlog_init(pulleys::RTC* rtc);        // after SD.begin()
void eventlog_record(const pulleys::MeshEvent& ev, bool relayed);
void eventlog_tick();                          // drain + periodic flush

bool     eventlog_ok();
uint32_t eventlog_rows();                      // rows written this session
uint32_t eventlog_dropped();                   // lost to a full queue or a bad card
const char* eventlog_path();

// Print the current log to serial. Lets the data be checked mid-event without
// pulling the card, which would stop the logging you came to inspect.
void eventlog_dump();
