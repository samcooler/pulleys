#include "eventlog.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include "clocksrc.h"

#define LOG_DIR        "/pulleys"
#define QUEUE_LEN      64
#define FLUSH_EVERY_MS 5000     // a power cut costs at most this much
#define FLUSH_EVERY_N  16

struct Row {
    uint32_t uptimeMs;
    pulleys::RtcTime when;
    bool     timeValid;
    uint16_t originId;
    uint8_t  channel;
    uint8_t  mode;
    uint8_t  magnitude;
    uint8_t  ttl;
    bool     relayed;
};

static Row      s_queue[QUEUE_LEN];
static volatile uint8_t s_head = 0, s_tail = 0;

static pulleys::RTC* s_rtc = nullptr;
static bool     s_ok       = false;
static char     s_path[48] = {0};
static uint32_t s_rows     = 0;
static uint32_t s_dropped  = 0;
static uint32_t s_lastFlush = 0;
static uint32_t s_sinceFlush = 0;
static File     s_file;

// A file per session rather than per day: the arbiter may reboot mid-event, and
// appending to one file after a reboot risks interleaving with a stale handle.
// Sessions are trivially concatenated afterwards; a corrupted shared file is
// not trivially recovered.
static void buildPath() {
    pulleys::RtcTime t;
    bool haveTime = s_rtc && s_rtc->present() && s_rtc->valid() &&
                    clocksrc_source() != TIME_NONE && s_rtc->now(t);
    if (haveTime) {
        snprintf(s_path, sizeof(s_path), LOG_DIR "/ev-%04u%02u%02u-%02u%02u%02u.csv",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        // No trustworthy clock: name by boot count so sessions stay distinct
        // and are obviously unsynced rather than pretending to a date.
        uint32_t n = 0;
        while (n < 1000) {
            snprintf(s_path, sizeof(s_path), LOG_DIR "/ev-unsynced-%03lu.csv",
                     (unsigned long)n);
            if (!SD.exists(s_path)) break;
            n++;
        }
    }
}

bool eventlog_init(pulleys::RTC* rtc) {
    s_rtc = rtc;
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
    buildPath();

    s_file = SD.open(s_path, FILE_WRITE);
    if (!s_file) {
        Serial.printf("  [LOG] cannot create %s — not logging\n", s_path);
        s_ok = false;
        return false;
    }
    s_file.println("iso_time,time_src,uptime_ms,origin_id,channel,mode,magnitude,ttl,relayed");
    s_file.flush();
    s_lastFlush = millis();
    s_ok = true;
    Serial.printf("  [LOG] writing %s\n", s_path);
    return true;
}

void eventlog_record(const pulleys::MeshEvent& ev, bool relayed) {
    uint8_t nh = (uint8_t)((s_head + 1) % QUEUE_LEN);
    if (nh == s_tail) { s_dropped++; return; }

    Row& r = s_queue[s_head];
    r.uptimeMs  = millis();
    r.timeValid = false;
    if (s_rtc && s_rtc->present() && s_rtc->valid() && s_rtc->now(r.when))
        r.timeValid = true;
    r.originId  = ev.originId;
    r.channel   = ev.channel;
    r.mode      = ev.mode;
    r.magnitude = ev.magnitude;
    r.ttl       = ev.ttl;
    r.relayed   = relayed;
    s_head = nh;
}

void eventlog_tick() {
    if (!s_ok) return;

    while (s_tail != s_head) {
        Row r = s_queue[s_tail];
        s_tail = (uint8_t)((s_tail + 1) % QUEUE_LEN);

        char ts[24];
        if (r.timeValid) pulleys::RTC::format(r.when, ts, sizeof(ts));
        else             snprintf(ts, sizeof(ts), "");

        size_t n = s_file.printf("%s,%s,%lu,%04X,%u,%s,%u,%u,%u\n",
                                 ts, clocksrc_source_str(),
                                 (unsigned long)r.uptimeMs, r.originId,
                                 r.channel,
                                 r.mode == pulleys::SENSOR_MODE_ROTATION ? "rot" : "lin",
                                 r.magnitude, r.ttl, r.relayed ? 1 : 0);
        if (n == 0) {
            // The card was pulled, or the write failed. Stop claiming to log.
            Serial.println("  [LOG] write failed — card removed? logging stopped");
            s_ok = false;
            s_file.close();
            return;
        }
        s_rows++;
        s_sinceFlush++;
    }

    uint32_t now = millis();
    if (s_sinceFlush >= FLUSH_EVERY_N || (now - s_lastFlush) >= FLUSH_EVERY_MS) {
        if (s_sinceFlush) s_file.flush();
        s_sinceFlush = 0;
        s_lastFlush  = now;
    }
}

void eventlog_dump() {
    if (!s_ok) { Serial.println("  [LOG] not logging — nothing to dump"); return; }
    s_file.flush();                      // pending rows first, or the tail is missing
    File r = SD.open(s_path, FILE_READ);
    if (!r) { Serial.println("  [LOG] cannot reopen for reading"); return; }
    Serial.printf("----- %s (%lu bytes) -----\n", s_path, (unsigned long)r.size());
    while (r.available()) Serial.write(r.read());
    Serial.println("----- end -----");
    r.close();
}

bool        eventlog_ok()      { return s_ok; }
uint32_t    eventlog_rows()    { return s_rows; }
uint32_t    eventlog_dropped() { return s_dropped; }
const char* eventlog_path()    { return s_path; }
