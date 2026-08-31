#include "clocksrc.h"
#include <Arduino.h>
#include <Preferences.h>

#define NVS_NS       "arbclock"
#define SNAPSHOT_MS  60000     // how much of an outage we can silently lose

static pulleys::RTC* s_rtc = nullptr;
static TimeSource    s_src = TIME_NONE;
static uint32_t      s_lastSnap = 0;

// "Aug 30 2026" / "22:58:00" — the compiler's format, not ours.
static bool parseBuildTime(pulleys::RtcTime& t) {
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day, year, hh, mm, ss;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;
    const char* p = strstr(months, mon);
    if (!p) return false;
    t.year = (uint16_t)year;
    t.month = (uint8_t)((p - months) / 3 + 1);
    t.day = (uint8_t)day;
    t.hour = (uint8_t)hh; t.minute = (uint8_t)mm; t.second = (uint8_t)ss;
    return true;
}

// Comparable ordering without pulling in time_t maths.
static uint64_t rank(const pulleys::RtcTime& t) {
    return ((uint64_t)t.year * 10000000000ULL) + ((uint64_t)t.month * 100000000ULL) +
           ((uint64_t)t.day * 1000000ULL) + ((uint64_t)t.hour * 10000ULL) +
           ((uint64_t)t.minute * 100ULL) + t.second;
}

static bool loadSnapshot(pulleys::RtcTime& t) {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return false;
    size_t n = p.getBytesLength("t");
    bool ok = (n == sizeof(pulleys::RtcTime)) &&
              (p.getBytes("t", &t, sizeof(t)) == sizeof(t));
    p.end();
    return ok;
}

static void saveSnapshot(const pulleys::RtcTime& t) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putBytes("t", &t, sizeof(t));
    p.end();
}

void clocksrc_init(pulleys::RTC* rtc) {
    s_rtc = rtc;
    s_lastSnap = millis();

    if (!rtc || !rtc->present()) {
        s_src = TIME_NONE;
        Serial.println("  [CLK] no RTC — rows will carry uptime only");
        return;
    }

    if (rtc->valid()) {
        // Kept power since it was last set; whatever set it still holds.
        s_src = TIME_SET;
        pulleys::RtcTime t; char b[24];
        rtc->now(t); pulleys::RTC::format(t, b, sizeof(b));
        Serial.printf("  [CLK] RTC running: %s\n", b);
        return;
    }

    // Lost power. Seed from the most recent evidence we have — a snapshot taken
    // before the outage, or the build time — whichever is later. Preferring the
    // later of the two is what stops an old firmware image from dragging the
    // clock backwards past a session that has already been logged.
    pulleys::RtcTime snap, build, chosen;
    bool haveSnap  = loadSnapshot(snap);
    bool haveBuild = parseBuildTime(build);

    if (haveSnap && haveBuild)      { chosen = rank(snap) >= rank(build) ? snap : build;
                                      s_src  = rank(snap) >= rank(build) ? TIME_RESTORED : TIME_BUILD; }
    else if (haveSnap)              { chosen = snap;  s_src = TIME_RESTORED; }
    else if (haveBuild)             { chosen = build; s_src = TIME_BUILD; }
    else                            { s_src = TIME_NONE;
                                      Serial.println("  [CLK] RTC unset and nothing to seed from");
                                      return; }

    rtc->set(chosen);
    char b[24];
    pulleys::RTC::format(chosen, b, sizeof(b));
    Serial.printf("  [CLK] RTC lost power — seeded %s from %s (approximate)\n",
                  b, clocksrc_source_str());
    Serial.println("  [CLK] set precisely with: T<YYYYMMDDHHMMSS>");
}

void clocksrc_tick() {
    if (!s_rtc || !s_rtc->present() || s_src == TIME_NONE) return;
    uint32_t now = millis();
    if (now - s_lastSnap < SNAPSHOT_MS) return;
    s_lastSnap = now;
    pulleys::RtcTime t;
    if (s_rtc->now(t) && t.year >= 2020) saveSnapshot(t);
}

void clocksrc_mark_set() {
    s_src = TIME_SET;
    pulleys::RtcTime t;
    if (s_rtc && s_rtc->now(t)) saveSnapshot(t);
}

TimeSource clocksrc_source() { return s_src; }

const char* clocksrc_source_str() {
    switch (s_src) {
        case TIME_SET:      return "set";
        case TIME_RESTORED: return "restored";
        case TIME_BUILD:    return "build";
        default:            return "none";
    }
}
