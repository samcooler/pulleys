#include <Arduino.h>
#include <pulleys_identity.h>
#include <pulleys_whoami.h>
#include <pulleys_protocol.h>
#include <pulleys_mesh.h>
#include <pulleys_ota.h>
#include <pulleys_census.h>
#include <pulleys_fw_serial.h>
#include <pulleys_fw_pipe.h>
#include <WebServer.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

// ── Bridge — mesh events out to a Raspberry Pi ────────────────────────────────
// Joins the mesh like any other node: it relays and answers census. On the
// Overlord's bridge it also serves node firmware over a same-channel AP.
//
// Every Pi in the piece -- the Overlord and any satellite players -- has a
// bridge, and they all run this image. Only one may host the firmware AP: a
// node told to update joins whichever `pulleys-fw` it finds, and a satellite's
// Pi has nothing to feed it. So the AP stays down until this bridge's own Pi
// starts firmware work (any FW- command, which only the Overlord's updater
// sends), and a satellite's bridge never raises it at all.
//
// What it adds is a second output. Every deduped sensor EVENT is written to the
// Pi as one text line:
//
//   EV <channel> <mode> <magnitude> <originId hex> <flags>\n
//
// e.g. "EV 3 1 90 A855 0". Fields are space-separated and parsed by position;
// anything new is appended.
//
// The arbiter's game fires cues, which arrive as the cue's name in ASCII and go
// out as
//
//   cue <name>\n
//   cue <name> enter\n
//
// e.g. "cue audio_once_chime". The second form is a cue the game fired on
// entering a state (MESH_CUE_FLAG_ENTER). The bridge is transparent: it holds
// no list of cues and checks nothing about the name, it passes on whatever it
// was given.
//
// What the game does, for the Pi's play log (MESH_MSG_GAME_INFO), goes out as
//
//   GI <key=value ...>\n
//
// e.g. "GI state=easy_round goals=red-blue". And the Pi may tune the game:
// a line "GAME-PARAM <name> <value>" from it goes onto the mesh as
// MESH_MSG_GAME_PARAM, for the arbiter, which clamps it to the game's range.
//
// The Pi link is the C3's USB port by default: one cable from the Pi powers the
// bridge and carries everything, and the Pi sees it as /dev/ttyACM0. That port
// is also the console, so in USB mode the two share one stream: every line the
// Pi sends is read once and routed by its prefix (FW- lines are the Pi's, the
// rest are console commands), and nothing the bridge prints to the console may
// look like a protocol line -- the cue echo, the "[PI]" echo and the census
// mirror are all off, since the Pi would read them twice.
//
// With -D BRIDGE_PI_USB=0 the link moves to UART1 on BRIDGE_TX_PIN/RX_PIN
// instead, and the USB port goes back to being a console alone: the wiring for
// a Pi on its GPIO UART.

#ifndef BRIDGE_PI_USB
  #define BRIDGE_PI_USB 1
#endif
#ifndef BRIDGE_TX_PIN
  #define BRIDGE_TX_PIN 19
#endif
#ifndef BRIDGE_RX_PIN
  #define BRIDGE_RX_PIN 18
#endif
#ifndef BRIDGE_BAUD
  #define BRIDGE_BAUD   pulleys::FW_SERIAL_BAUD
#endif
// Ask the Pi for firmware chunk N+1 while chunk N is still going out over WiFi
// (see pulleys_fw_pipe.h): about a third faster than one at a time. -D BRIDGE_PIPELINE=0
// goes back to strictly one at a time, as a fallback if it ever misbehaves. It does
// NOT apply to the bridge's own flash update, which must never overlap serial
// receive with a flash write (see updateBridgeSelf).
#ifndef BRIDGE_PIPELINE
  #define BRIDGE_PIPELINE 1
#endif

// Where the Pi's lines go, and whether the console shares that port.
#if BRIDGE_PI_USB
  #define PI_LINK Serial
#else
  #define PI_LINK Serial1
#endif
static constexpr bool PI_SHARES_CONSOLE = BRIDGE_PI_USB;

#define QUEUE_LEN 32   // events waiting for the Pi link; a burst of sensors fits

static WebServer fwServer(80);
static char fwSha[65] = {};
static char fwBuildHash[65] = {};
static char fwMd5[33] = {};
static char fwEnv[32] = {};
static uint32_t fwSize = 0;
static uint16_t fwTarget = 0;
static bool fwOfferActive = false;
// Shared by the HTTP stream and self-update, which run one at a time. These
// buffers cannot live on the 8 KiB Arduino loop stack with 4 KiB chunks.
static uint8_t fwChunkBuffer[pulleys::FW_CHUNK_BYTES];
static uint8_t fwChunkBuffer2[pulleys::FW_CHUNK_BYTES];   // the chunk arriving while fwChunkBuffer's goes out
static char fwChunkLine[pulleys::FW_LINE_BYTES];
// The Pi has told us to stop (it cancelled the update): do not retry, give up now.
static bool fwRefused = false;
// Diagnostic: where one chunk's time goes (microseconds, summed per transfer, printed by
// serveFirmware): request sent -> first byte back, first byte -> end of the line, line -> parsed.
static uint32_t fwRequestedUs = 0, fwLatencyUs = 0, fwStreamUs = 0, fwParseUs = 0, fwTimedChunks = 0;
// Bytes from the Pi are read in blocks, not one at a time. available()+read() per byte each
// take a lock in the serial driver (about 27 us a byte measured here), which is slower than
// the wire at 921600 baud: a 5.5 KB chunk took 153 ms to read where it needs 61 ms on the
// wire. Bytes read but not yet used stay here between calls, so none is lost.
static uint8_t fwRx[512];
static size_t fwRxHead = 0, fwRxTail = 0;
static void drainQueue();

// Ask the Pi for a chunk. Does not wait: the answer is read by receiveFirmwareChunk,
// and meanwhile the serial driver buffers it on its own.
static void sendFirmwareChunkRequest(uint32_t offset, size_t want) {
    fwRequestedUs = micros();
    PI_LINK.printf("FW-READ sha256=%s offset=%lu length=%u\n", fwSha,
                   (unsigned long)offset, (unsigned)want);
}

// The next byte from the Pi, or -1 if none is waiting.
static int fwReadByte() {
    if (fwRxHead == fwRxTail) {
        size_t waiting = PI_LINK.available();
        if (!waiting) return -1;
        fwRxTail = PI_LINK.readBytes(fwRx, waiting < sizeof(fwRx) ? waiting : sizeof(fwRx));
        fwRxHead = 0;
        if (!fwRxTail) return -1;
    }
    return fwRx[fwRxHead++];
}

// Wait for the answer to a request for `offset`, and check it (base64, length, CRC).
static bool receiveFirmwareChunk(uint32_t offset, uint8_t* out, size_t want, size_t& got) {
    char* line = fwChunkLine;
    uint32_t start = millis();
    size_t used = 0;
    uint32_t firstByteUs = 0, lineEndUs = 0;
    while (millis() - start < 5000) {
        int next;
        while ((next = fwReadByte()) >= 0) {
            if (!firstByteUs) firstByteUs = micros();
            char c = (char)next;
            if (c == '\n' || c == '\r') {
                if (!used) continue;
                lineEndUs = micros();
                line[used] = 0;
                if (strncmp(line, "FW-DATA ", 8) == 0) {
                    char* off = strstr(line, "offset=");
                    char* b64 = strstr(line, "b64=");
                    if (!off || !b64 || !strstr(line, fwSha) ||
                        strtoul(off + 7, nullptr, 10) != offset) {
                        // A retry can leave a second response in flight. It
                        // belongs to the old offset, not to another retry of
                        // this chunk; keep waiting for our matching response.
                        used = 0;
                        continue;
                    }
                    b64 += 4;
                    size_t decoded = 0;
                    int rc = mbedtls_base64_decode(out, want, &decoded,
                                                  (const unsigned char*)b64, strlen(b64));
                    if (rc != 0 || decoded != want) return false;
                    char* check = strstr(line, "crc32=");
                    if (!check) return false;
                    char* end = nullptr;
                    uint32_t expected = strtoul(check + 6, &end, 16);
                    if (end != check + 14 || *end != ' ' ||
                        pulleys::fw_chunk_crc32(out, decoded) != expected) return false;
                    got = decoded;
                    fwLatencyUs += firstByteUs - fwRequestedUs;
                    fwStreamUs += lineEndUs - firstByteUs;
                    fwParseUs += micros() - lineEndUs;
                    ++fwTimedChunks;
                    return true;
                }
                if (strncmp(line, "FW-ERROR ", 9) == 0 && strstr(line, fwSha)) {
                    // The Pi cannot or will not serve this chunk. If it is the one we are
                    // waiting for, stop waiting at once instead of timing out; a cancel is final.
                    const char* off = strstr(line, "offset=");
                    if (off && strtoul(off + 7, nullptr, 10) == offset) {
                        if (strstr(line, "reason=cancelled")) fwRefused = true;
                        return false;
                    }
                }
                used = 0;
            } else if (used < pulleys::FW_LINE_BYTES - 1) {
                line[used++] = c;
            }
        }
        pulleys::mesh_poll();
        drainQueue();
        delay(1);
    }
    Serial.printf("[FW] chunk response timed out at offset=%lu partial=%u\n",
                  (unsigned long)offset, (unsigned)used);
    return false;
}

static bool requestFirmwareChunkOnce(uint32_t offset, uint8_t* out, size_t want, size_t& got) {
    sendFirmwareChunkRequest(offset, want);
    return receiveFirmwareChunk(offset, out, want, got);
}

static bool requestFirmwareChunk(uint32_t offset, uint8_t* out, size_t want, size_t& got) {
    // Chunks are addressed by offset, so requesting one again is safe. Recover
    // from a lost USB control record before the node's HTTP read times out.
    for (uint8_t attempt = 0; attempt < 3 && !fwRefused; ++attempt) {
        got = 0;
        if (requestFirmwareChunkOnce(offset, out, want, got) && got == want) return true;
    }
    return false;
}

// The serial link to the Pi, as pulleys::ChunkPipe wants it (see pulleys_fw_pipe.h).
struct FirmwareLink {
    void request(uint32_t offset, size_t want) { sendFirmwareChunkRequest(offset, want); }
    bool receive(uint32_t offset, uint8_t* out, size_t want) {
        size_t got = 0;
        return receiveFirmwareChunk(offset, out, want, got) && got == want;
    }
    bool fetch(uint32_t offset, uint8_t* out, size_t want) {
        size_t got = 0;
        return requestFirmwareChunk(offset, out, want, got) && got == want;
    }
};

static void serveFirmware() {
    String requested = fwServer.uri();
    const String prefix = "/api/v1/firmware/";
    if (requested.startsWith(prefix)) requested = requested.substring(prefix.length());
    if (!fwOfferActive || (requested != fwSha && requested != "/firmware.bin") || fwSize == 0) {
        fwServer.send(404, "text/plain", "no matching firmware offer");
        return;
    }
    fwServer.setContentLength(fwSize);
    fwServer.send(200, "application/octet-stream", "");
    WiFiClient client = fwServer.client();
    fwRefused = false;
    FirmwareLink link;
    pulleys::ChunkPipe<FirmwareLink> pipe(link, fwChunkBuffer, fwChunkBuffer2,
                                          pulleys::FW_CHUNK_BYTES, fwSize, BRIDGE_PIPELINE);
    uint32_t offset = 0;
    // Where does the time go? Summed per transfer and printed at the end: waiting on the
    // serial link for chunks (first, and every next()), versus blocked writing to the device.
    uint32_t began = millis(), firstMs = 0, waitMs = 0, writeMs = 0, maxWriteMs = 0, chunks = 0, writes = 0;
    fwLatencyUs = fwStreamUs = fwParseUs = fwTimedChunks = 0;
    uint32_t mark = millis();
    bool started = pipe.first();
    firstMs = millis() - mark;
    if (started) {
        do {
            pipe.prefetch();   // the next chunk starts arriving over the serial link while this one goes out over WiFi
            size_t sent = 0;
            mark = millis();
            while (sent < pipe.length() && client.connected()) {
                size_t n = client.write(pipe.data() + sent, pipe.length() - sent);
                ++writes;
                if (!n) break;
                sent += n;
            }
            uint32_t took = millis() - mark;
            writeMs += took;
            if (took > maxWriteMs) maxWriteMs = took;
            ++chunks;
            if (sent != pipe.length()) break;
            offset += sent;
            mark = millis();
            bool more = client.connected() && pipe.next();
            waitMs += millis() - mark;
            if (!more) break;
        } while (true);
    }
    Serial.printf("[FW] HTTP served %lu/%lu bytes\n", (unsigned long)offset,
                  (unsigned long)fwSize);
    Serial.printf("[FW] timing: %lu ms total, first chunk %lu ms, serial wait %lu ms, device write %lu ms "
                  "(max %lu ms), %lu chunks, %lu write calls, pipe=%d\n",
                  (unsigned long)(millis() - began), (unsigned long)firstMs, (unsigned long)waitMs,
                  (unsigned long)writeMs, (unsigned long)maxWriteMs, (unsigned long)chunks,
                  (unsigned long)writes, (int)BRIDGE_PIPELINE);
    if (fwTimedChunks)
        Serial.printf("[FW] per chunk: request->first byte %.1f ms, first byte->line end %.1f ms, parse %.1f ms (%lu chunks timed)\n",
                      fwLatencyUs / 1000.0 / fwTimedChunks, fwStreamUs / 1000.0 / fwTimedChunks,
                      fwParseUs / 1000.0 / fwTimedChunks, (unsigned long)fwTimedChunks);
}

// Bench-test the same checked transfer path without writing an OTA partition.
static void benchmarkFirmware(char* command) {
    char digest[65] = {};
    unsigned size = 0;
    if (fwOfferActive || sscanf(command, "sha256=%64s size=%u", digest, &size) != 2 ||
        strlen(digest) != 64 || !size || size > 7 * 1024 * 1024) return;
    char previous[65]; strlcpy(previous, fwSha, sizeof(previous));
    strlcpy(fwSha, digest, sizeof(fwSha));
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts_ret(&hash, 0);
    uint32_t started = millis(), offset = 0;
    fwRefused = false;
    FirmwareLink link;
    pulleys::ChunkPipe<FirmwareLink> pipe(link, fwChunkBuffer, fwChunkBuffer2,
                                          pulleys::FW_CHUNK_BYTES, size, BRIDGE_PIPELINE);
    if (pipe.first()) {
        do {
            pipe.prefetch();
            mbedtls_sha256_update_ret(&hash, pipe.data(), pipe.length());
            offset += pipe.length();
        } while (pipe.next());
    }
    uint8_t sum[32]; char actual[65];
    mbedtls_sha256_finish_ret(&hash, sum);
    mbedtls_sha256_free(&hash);
    for (uint8_t i = 0; i < 32; ++i) snprintf(actual + 2 * i, 3, "%02x", sum[i]);
    PI_LINK.printf("FW-BENCH state=%s bytes=%lu elapsed_ms=%lu sha256=%s\n",
                   offset == size ? "ok" : "failed", (unsigned long)offset,
                   (unsigned long)(millis() - started), actual);
    strlcpy(fwSha, previous, sizeof(fwSha));
}

static void onFirmwareStatus(const pulleys::MeshFwStatus& status) {
    if (status.originId != fwTarget) return;
    const char* state = status.state == pulleys::FW_OK ? "ok"
                      : status.state == pulleys::FW_FAILED ? "failed"
                      : status.state == pulleys::FW_DOWNLOADING ? "downloading" : "ready";
    PI_LINK.printf("FW-STATUS id=%04X env=%s state=%s build_hash=%s sha256=%s err=%d\n",
                   status.originId, status.env, state, status.buildHash,
                   status.sha256, (int)status.error);
    if (status.state == pulleys::FW_OK) fwOfferActive = false;
}

static void onLegacyOtaStatus(const pulleys::MeshOtaReady& status) {
    if (status.originId != fwTarget) return;
    const char* state = status.state == pulleys::OTA_STATE_OK ? "ok"
                      : status.state == pulleys::OTA_STATE_FAILED ? "failed"
                      : status.state == pulleys::OTA_STATE_DOWNLOADING ? "downloading" : "ready";
    PI_LINK.printf("FW-STATUS id=%04X env=%s state=%s build_hash=%s sha256=%s err=%d\n",
                   status.originId, status.env, state, fwBuildHash, fwSha,
                   (int)status.err);
    if (status.state == pulleys::OTA_STATE_OK) fwOfferActive = false;
}

static void updateBridgeSelf(uint32_t size, const char* sha) {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!size || strlen(sha) != 64 || !next || size > next->size) {
        PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=failed err=bad_image\n",
                       pulleys::identity_id());
        return;
    }
    strlcpy(fwSha, sha, sizeof(fwSha));
    fwSize = size;
    if (!Update.begin(size, U_FLASH)) {
        PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=failed err=%d\n",
                       pulleys::identity_id(), (int)Update.getError());
        return;
    }
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts_ret(&hash, 0);
    uint32_t off = 0;
    fwRefused = false;
    FirmwareLink link;
    // Never pipelined, whatever BRIDGE_PIPELINE says: Update.write erases and writes a
    // flash sector, and the serial receive interrupt is not in IRAM in this SDK
    // (CONFIG_UART_ISR_IN_IRAM is off), so it cannot run during that. A prefetched answer
    // arriving then overflows the 128-byte UART FIFO and is lost: every chunk timed out
    // for 5 s before the retry got it through (0.6 KB/s). Ask, receive it whole, THEN write.
    pulleys::ChunkPipe<FirmwareLink> pipe(link, fwChunkBuffer, fwChunkBuffer2,
                                          pulleys::FW_CHUNK_BYTES, size, false);
    bool ok = pipe.first();
    if (ok) {
        do {
            pipe.prefetch();   // a no-op here: the pipe is not pipelined
            size_t got = pipe.length();
            if (Update.write(pipe.data(), got) != got) { ok = false; break; }
            mbedtls_sha256_update_ret(&hash, pipe.data(), got);
            off += got;
            if (off == size || off / 65536 != (off - got) / 65536)
                PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=downloading offset=%lu\n",
                               pulleys::identity_id(), (unsigned long)off);
        } while (pipe.next());
        if (pipe.failed()) ok = false;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&hash, digest);
    mbedtls_sha256_free(&hash);
    char actual[65];
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[64] = 0;
    if (!ok || off != size || strcmp(actual, sha) != 0) {
        Update.abort();
        PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=failed err=hash\n",
                       pulleys::identity_id());
        return;
    }
    if (!Update.end(true)) {
        PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=failed err=write\n",
                       pulleys::identity_id());
        return;
    }
    Preferences prefs;
    if (prefs.begin("fw", false)) { prefs.putString("sha256", sha); prefs.end(); }
    PI_LINK.printf("FW-STATUS id=%04X env=" PULLEYS_ENV " state=ok sha256=%s\n",
                   pulleys::identity_id(), sha);
    Serial.flush();
    if (!PI_SHARES_CONSOLE) PI_LINK.flush();
    delay(500);
    ESP.restart();
}

struct QueuedEvent {
    uint8_t  channel, mode, magnitude, flags;
    uint16_t originId;
};
static QueuedEvent queue[QUEUE_LEN];
static uint8_t     qHead = 0, qTail = 0;

struct QueuedCue { char name[pulleys::MESH_CUE_NAME_MAX + 1]; uint8_t flags; };
static QueuedCue   cueQueue[QUEUE_LEN];
static uint8_t     cHead = 0, cTail = 0;

// The console is for reading what the bridge is doing, and by default that is
// the cues and nothing else. `v` brings back the per-event lines and the 3 s
// counters, for debugging the radio.
static bool verbose = false;

static uint32_t forwarded = 0;
static uint32_t dropped   = 0;
static uint32_t cuesSent  = 0;

// The mesh callback only queues. The write to the Pi happens in loop(), so a
// slow or unplugged Pi can never stall the radio path.
static void onMeshEvent(const pulleys::MeshEvent& ev, bool relayed) {
    if (!pulleys::mesh_event_is_pull(ev)) return;   // a sensor confirming a remap
    uint8_t next = (qHead + 1) % QUEUE_LEN;
    if (next == qTail) { dropped++; return; }
    queue[qHead] = { ev.channel, ev.mode, ev.magnitude, ev.flags, ev.originId };
    qHead = next;
    if (verbose)
        Serial.printf("★ ch%-2d from 0x%04X %s mag=%3d\n", ev.channel, ev.originId,
                      relayed ? "(relayed)" : "(direct) ", ev.magnitude);
}

static void onMeshCue(const pulleys::MeshCue& c, bool /*relayed*/) {
    uint8_t next = (cHead + 1) % QUEUE_LEN;
    if (next == cTail) { dropped++; return; }
    strlcpy(cueQueue[cHead].name, c.name, sizeof(cueQueue[cHead].name));
    cueQueue[cHead].flags = c.flags;
    cHead = next;
}

// The game's telemetry, a line each. Bodies are short ASCII; one a loop pass
// at most from the arbiter, so a few slots are plenty.
static const uint8_t INFO_LEN = 6;
static char    infoQueue[INFO_LEN][pulleys::MESH_FLOOD_MAX_BODY + 1];
static uint8_t iHead = 0, iTail = 0;

static void onGameInfo(const pulleys::MeshFlood& f, bool /*relayed*/) {
    uint8_t next = (iHead + 1) % INFO_LEN;
    if (next == iTail) { dropped++; return; }
    uint8_t n = f.len <= pulleys::MESH_FLOOD_MAX_BODY ? f.len : pulleys::MESH_FLOOD_MAX_BODY;
    memcpy(infoQueue[iHead], f.body, n);
    infoQueue[iHead][n] = 0;
    for (uint8_t i = 0; i < n; i++)   // one line, whatever arrived
        if (infoQueue[iHead][i] == '\n' || infoQueue[iHead][i] == '\r') infoQueue[iHead][i] = ' ';
    iHead = next;
}

// Configuration replies have their own bounded queue. Keep callbacks free of
// UART writes, and leave normal events/cues in their existing queues.
static constexpr uint8_t INSTALL_QUEUE = 8;
static char installQueue[INSTALL_QUEUE][512];
static uint8_t installHead = 0, installTail = 0;
static void onInstallation(const pulleys::MeshFlood& f, bool) {
    if (f.h.originType != pulleys::MESH_ORIGIN_ARBITER || f.len < 28) return;
    char body[pulleys::MESH_FLOOD_MAX_BODY + 1];
    memcpy(body, f.body, f.len); body[f.len] = 0;
    char q[17]; unsigned bridge, part, count;
    if (sscanf(body, "%16s %x %u %u", q, &bridge, &part, &count) != 4 ||
        bridge != pulleys::identity_id() || count < 1 || count > 64 || part >= count) return;
    for (uint8_t i = 0; i < f.len; ++i)
        if (body[i] == '\r' || body[i] == '\n') return;
    uint8_t next = (installHead + 1) % INSTALL_QUEUE;
    if (next == installTail) { ++dropped; return; }
    char* out = installQueue[installHead];
    int used = snprintf(out, 512, "IC %04X %.28s", f.h.originId, body);
    // Hex protects whitespace at chunk boundaries from line-reader trimming.
    for (uint8_t i = 28; i < f.len; ++i)
        used += snprintf(out + used, 512 - used, "%02X", (uint8_t)body[i]);
    out[used++] = '\n'; out[used] = 0;
    installHead = next;
}

static void drainQueue() {
    while (iTail != iHead) {
        if (PI_LINK.availableForWrite() < 260) break;
        PI_LINK.printf("GI %s\n", infoQueue[iTail]);
        iTail = (iTail + 1) % INFO_LEN;
    }


    while (cTail != cHead) {
        if (PI_LINK.availableForWrite() < 80) break;
        const char* name = cueQueue[cTail].name;
        const char* tail = (cueQueue[cTail].flags & pulleys::MESH_CUE_FLAG_ENTER) ? " enter" : "";
        PI_LINK.printf("cue %s%s\n", name, tail);
        if (!PI_SHARES_CONSOLE) Serial.printf("cue %s%s\n", name, tail);
        cTail = (cTail + 1) % QUEUE_LEN;
        cuesSent++;
    }

    while (qTail != qHead) {
        // Leave the event queued if the link would block; try again next loop.
        if (PI_LINK.availableForWrite() < 32) return;
        const QueuedEvent& q = queue[qTail];
        PI_LINK.printf("EV %u %u %u %04X %u\n", q.channel, q.mode, q.magnitude,
                       q.originId, q.flags);
        qTail = (qTail + 1) % QUEUE_LEN;
        forwarded++;
    }
    // Media and sensor records take priority over configuration readback.
    while (installTail != installHead) {
        size_t length = strlen(installQueue[installTail]);
        if ((size_t)PI_LINK.availableForWrite() < length) break;
        PI_LINK.write((const uint8_t*)installQueue[installTail], length);
        installTail = (installTail + 1) % INSTALL_QUEUE;
    }

}

// The firmware AP and its HTTP endpoint. Raised once, the first time this
// bridge's Pi speaks the firmware protocol; see the note at the top.
static bool fwHosting = false;

static void startFirmwareHost() {
    if (fwHosting) return;
#if BRIDGE_PI_USB && CONFIG_IDF_TARGET_ESP32C3
    // Firmware control records must arrive whole. The idle console drops output
    // rather than waiting for a missing host, but a dropped FW-READ stalls the
    // download. The Pi has now contacted us, so allow bounded USB backpressure.
    Serial.setTxTimeoutMs(1000);
#endif
    if (!WiFi.mode(WIFI_AP_STA) ||
        !WiFi.softAP("pulleys-fw", "", MESH_WIFI_CHANNEL, 0, 4)) {
        Serial.println("[FW] firmware AP failed to start");
        return;
    }
    fwHosting = true;
    fwServer.on("/api/v1/health", HTTP_GET, []() {
        fwServer.send(200, "application/json",
                      String("{\"channel\":") + MESH_WIFI_CHANNEL +
                      ",\"busy\":" + (fwOfferActive ? "true" : "false") + "}");
    });
    // Nodes on the retired OTA client request this path during migration.
    fwServer.on("/firmware.bin", HTTP_GET, serveFirmware);
    fwServer.onNotFound([]() {
        if (fwServer.method() == HTTP_GET && fwServer.uri().startsWith("/api/v1/firmware/")) {
            serveFirmware();
            return;
        }
        fwServer.send(404, "text/plain", "not found");
    });
    fwServer.begin();
    Serial.printf("  Firmware AP: pulleys-fw ch=%d ip=%s (this Pi is the Overlord)\n",
                  MESH_WIFI_CHANNEL, WiFi.softAPIP().toString().c_str());
}

// One line from the Pi: the firmware protocol. Echoed to the console when the
// console is a port of its own, so it can be read there.
static void handlePiLine(char* buf) {
    if (!PI_SHARES_CONSOLE) Serial.printf("  [PI] %s\n", buf);
    if (strncmp(buf, "INSTALL ", 8) == 0) {
        size_t n = strlen(buf + 8);
        if (n > 0 && n <= pulleys::MESH_FLOOD_MAX_BODY)
            pulleys::mesh_send_flood(pulleys::MESH_MSG_INSTALL_REQ, buf + 8, n, 1);
        return;
    }
    if (strncmp(buf, "GAME-PARAM ", 11) == 0) {
        // "GAME-PARAM round_time 90": for the arbiter, as "<name> <value>"
        const char* body = buf + 11;
        size_t n = strlen(body);
        if (n > 0 && n < 64)
            pulleys::mesh_send_flood(pulleys::MESH_MSG_GAME_PARAM, body, (uint8_t)n, 3, 120);
        return;
    }
    if (strncmp(buf, "FW-BENCH ", 9) == 0) { benchmarkFirmware(buf + 9); return; }
    if (strncmp(buf, "FW-", 3) == 0) startFirmwareHost();
    if (strncmp(buf, "FW-PING", 7) == 0) {
        PI_LINK.printf("FW-BRIDGE id=%04X env=" PULLEYS_ENV " sha256=%s baud=%lu chunk=%u pipe=%d\n",
                       pulleys::identity_id(), pulleys::whoami_firmware_hash(), (unsigned long)BRIDGE_BAUD, (unsigned)pulleys::FW_CHUNK_BYTES, (int)BRIDGE_PIPELINE);
    } else if (strncmp(buf, "FW-CENSUS", 9) == 0 ||
               strncmp(buf, "FW-QUERY", 8) == 0) {
        int spread = 3000;
        char* at = strstr(buf, "spread=");
        if (at) spread = atoi(at + 7);
        if (spread < 500 || spread > 10000) spread = 3000;
        pulleys::census_request((uint16_t)spread);
    } else if (strncmp(buf, "FW-OFFER ", 9) == 0) {
        char id[8] = {}, env[32] = {}, buildHash[65] = {}, sha[65] = {}, md5[33] = {};
        unsigned size = 0;
        if (sscanf(buf + 9, "id=%7s env=%31s build_hash=%64s sha256=%64s md5=%32s size=%u",
                   id, env, buildHash, sha, md5, &size) == 6 &&
            strlen(buildHash) == 64 && strlen(sha) == 64 && strlen(md5) == 32 && size > 0) {
            fwTarget = (uint16_t)strtoul(id, nullptr, 16);
            strlcpy(fwEnv, env, sizeof(fwEnv));
            strlcpy(fwBuildHash, buildHash, sizeof(fwBuildHash));
            strlcpy(fwSha, sha, sizeof(fwSha));
            strlcpy(fwMd5, md5, sizeof(fwMd5));
            fwSize = size;
            fwOfferActive = true;
            pulleys::mesh_send_ota_announce("pulleys-fw", "", fwEnv,
                                            fwTarget, 300, fwMd5);
            pulleys::mesh_send_fw_offer(fwTarget, fwEnv, fwBuildHash, fwSha, fwSize);
            PI_LINK.printf("FW-STATUS id=%04X env=%s state=offered sha256=%s err=0\n",
                           fwTarget, fwEnv, fwSha);
        } else {
            PI_LINK.println("FW-ERROR reason=bad_offer");
        }
    } else if (strncmp(buf, "FW-BRIDGE-UPDATE ", 17) == 0) {
        char imageEnv[32] = {};
        char sha[65] = {};
        unsigned size = 0;
        char* shaAt = strstr(buf, "sha256=");
        char* sizeAt = strstr(buf, "size=");
        if (sscanf(buf + 17, "env=%31s", imageEnv) == 1 &&
            strcmp(imageEnv, PULLEYS_ENV) == 0 &&
            shaAt && sizeAt && sscanf(shaAt, "sha256=%64s", sha) == 1 &&
            sscanf(sizeAt, "size=%u", &size) == 1)
            updateBridgeSelf(size, sha);
        else PI_LINK.println("FW-STATUS state=failed err=bad_command");
    }
}

// ── Serial console ────────────────────────────────────────────────────────────
//   e<ch>  inject a fake event on that channel, as if the mesh had delivered it
//   v      toggle verbose: per-event lines and the 3 s counters
//   c <name>  inject a fake cue, e.g. "c audio_once_chime"; "c <name> enter" as an enter cue
static void handleConsoleLine(char* buf) {
    if (pulleys::whoami_handle(buf)) return;
    if (pulleys::census_handle_serial(buf)) return;
    if (buf[0] == 'v' && buf[1] == 0) {
        verbose = !verbose;
        Serial.printf("verbose %s\n", verbose ? "on" : "off");
    } else if (buf[0] == 'c' && buf[1] == ' ') {
        pulleys::MeshCue c = {};
        char* sp = strchr(buf + 2, ' ');
        if (sp) {
            *sp = 0;
            if (strcmp(sp + 1, "enter") == 0) c.flags = pulleys::MESH_CUE_FLAG_ENTER;
        }
        c.nameLen = (uint8_t)strlcpy(c.name, buf + 2, sizeof(c.name));
        onMeshCue(c, false);
    } else if (buf[0] == 'e') {
        pulleys::MeshEvent ev = {};
        ev.channel   = (uint8_t)atoi(buf + 1);
        ev.mode      = pulleys::SENSOR_MODE_LINEAR;
        ev.magnitude = 90;
        ev.originId  = pulleys::identity_id();
        ev.ttl       = pulleys::MESH_TTL_START;
        onMeshEvent(ev, false);
    }
}

// Split a stream into lines and hand each one on. Sized for the firmware
// protocol's longest line, an FW-DATA chunk in base64.
struct LineReader {
    char     buf[pulleys::FW_LINE_BYTES];
    uint16_t len = 0;
    template <typename S, typename F> void poll(S& in, F handle) {
        while (in.available()) {
            char c = (char)in.read();
            if (c == '\n' || c == '\r') {
                if (!len) continue;
                buf[len] = 0;
                len = 0;
                handle(buf);
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = c;
            }
        }
    }
};
static LineReader consoleIn;
#if !BRIDGE_PI_USB
static LineReader piIn;
#endif

// The console -- and in USB mode the Pi too, on the same port: FW- and GAME-
// lines are the Pi's, everything else a console command.
static void handleSerial() {
    consoleIn.poll(Serial, [](char* line) {
        if (PI_SHARES_CONSOLE && (strncmp(line, "FW-", 3) == 0 || strncmp(line, "GAME-", 5) == 0 || strncmp(line, "INSTALL ", 8) == 0))
            handlePiLine(line);
        else handleConsoleLine(line);
    });
}

static void pollPi() {
#if !BRIDGE_PI_USB
    piIn.poll(Serial1, handlePiLine);
#endif
}

void setup() {
#if BRIDGE_PI_USB
    // A 4 KiB chunk is about 5.6 KiB of Base64 plus framing. Configure
    // hardware buffers before begin(); a delayed loop must not truncate it.
    Serial.setRxBufferSize(pulleys::FW_SERIAL_RX_BYTES);
    Serial.setTxBufferSize(1024);
#if CONFIG_IDF_TARGET_ESP32C3
    // Keep an idle native-USB console from stalling the radio without a host.
    Serial.setTxTimeoutMs(0);
#endif
#else
    Serial1.setRxBufferSize(pulleys::FW_SERIAL_RX_BYTES);
    Serial1.setTxBufferSize(1024);
    Serial1.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);
#endif
#if BRIDGE_PI_USB
    Serial.begin(BRIDGE_BAUD);
#else
    Serial.begin(115200); // separate bench console; Pi UART uses BRIDGE_BAUD
#endif
    delay(1000);

    pulleys::identity_init(PULLEYS_TYPE_BRIDGE);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("  PULLEYS Bridge  %s\n", pulleys::identity_name());
#if BRIDGE_PI_USB
    Serial.println("  Pi link: USB (shared with this console)");
#else
    Serial.printf("  Pi link: UART1 tx=%d rx=%d @ %d\n",
                  BRIDGE_TX_PIN, BRIDGE_RX_PIN, BRIDGE_BAUD);
#endif
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    randomSeed(esp_random());
    pulleys::mesh_init(pulleys::MESH_ORIGIN_BRIDGE, pulleys::identity_id());
    pulleys::ota_init();
    pulleys::census_init();
    // Census lines already go to Serial; a mirror onto the same port would hand
    // the Pi every one twice.
    if (!PI_SHARES_CONSOLE) pulleys::census_set_mirror(&PI_LINK);
    pulleys::mesh_on_fw_status(onFirmwareStatus);
    pulleys::mesh_on_ota_ready(onLegacyOtaStatus);
    pulleys::mesh_on_event(onMeshEvent);
    pulleys::mesh_on_cue(onMeshCue);
    pulleys::mesh_on_flood(pulleys::MESH_MSG_GAME_INFO, onGameInfo);
    pulleys::mesh_on_flood(pulleys::MESH_MSG_INSTALL_RES, onInstallation);

    pulleys::census_set_detail(BRIDGE_PI_USB ? "usb" : "uart1");
    pulleys::whoami_reply();
    PI_LINK.printf("FW-BRIDGE id=%04X env=" PULLEYS_ENV " sha256=%s baud=%lu chunk=%u pipe=%d\n",
                   pulleys::identity_id(), pulleys::whoami_firmware_hash(), (unsigned long)BRIDGE_BAUD, (unsigned)pulleys::FW_CHUNK_BYTES, (int)BRIDGE_PIPELINE);
    Serial.println("Bridge ready — forwarding mesh events to the Pi.\n");
}

void loop() {
    static uint32_t lastLog = 0;
    uint32_t now = millis();

    pulleys::mesh_poll();
    pulleys::census_poll();
    if (fwHosting) fwServer.handleClient();

    // A targeted node offer owns this node while it downloads and installs.
    if (pulleys::ota_active()) {
        pulleys::ota_poll();
        handleSerial();
        return;
    }

    handleSerial();
    drainQueue();
    pollPi();

    if (verbose && now - lastLog >= 3000) {
        lastLog = now;
        Serial.printf("── forwarded=%lu dropped=%lu cues=%lu\n",
                      (unsigned long)forwarded, (unsigned long)dropped,
                      (unsigned long)cuesSent);
        pulleys::mesh_print_stats();
    }
}
