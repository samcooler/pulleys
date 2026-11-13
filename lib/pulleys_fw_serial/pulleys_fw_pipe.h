#pragma once
#include <stddef.h>
#include <stdint.h>

// ── Double-buffered firmware chunk fetch ─────────────────────────────────────
//
// The bridge pulls a firmware image from the Overlord one chunk at a time over
// its serial link, and hands each chunk on (over WiFi to a node, or to its own
// flash). Done strictly in turn -- ask, wait for the whole chunk to arrive, hand
// it on, ask again -- the serial transfer and the hand-off add up. This asks for
// chunk N+1 *before* handing on chunk N, so the next chunk is arriving over the
// serial link (into its receive buffer, which the hardware fills on its own)
// while the current one is going out. The cycle is then the slower of the two
// stages, not their sum.
//
//     ChunkPipe<Link> pipe(link, bufferA, bufferB, FW_CHUNK_BYTES, imageSize);
//     if (!pipe.first()) fail;
//     do {
//         pipe.prefetch();                       // chunk N+1 starts arriving now
//         hand_on(pipe.data(), pipe.length());   // chunk N
//     } while (pipe.next());                     // waits for N+1 if it is still coming
//     if (pipe.failed()) fail;
//
// `Link` is the transport, so this can be tested on a computer with a fake one:
//
//     void request(uint32_t offset, size_t want);                    // ask, do not wait
//     bool receive(uint32_t offset, uint8_t* out, size_t want);      // wait for that answer; true only for all `want` bytes
//     bool fetch(uint32_t offset, uint8_t* out, size_t want);        // ask and wait, retrying; true only for all `want` bytes
//
// Chunks are addressed by offset, so asking again is always safe. If the answer
// to a prefetch does not come (or is damaged), next() simply asks again with
// fetch(), so pipelining can only ever fall back to the one-at-a-time behaviour.
// Built with pipelined = false it never prefetches: exactly that one-at-a-time
// behaviour (the BRIDGE_PIPELINE=0 fallback).
namespace pulleys {

template <typename Link>
class ChunkPipe {
public:
    ChunkPipe(Link& link, uint8_t* a, uint8_t* b, size_t chunk, uint32_t total, bool pipelined = true)
        : link_(link), cur_(a), nxt_(b), chunk_(chunk), total_(total), pipelined_(pipelined) {}

    // Fetch the first chunk. False if the image is empty or the fetch failed.
    bool first() {
        offset_ = 0;
        length_ = 0;
        if (total_ == 0) return false;
        size_t want = wantAt(0);
        if (!link_.fetch(0, cur_, want)) { failed_ = true; return false; }
        length_ = want;
        return true;
    }

    uint8_t* data() const { return cur_; }
    size_t length() const { return length_; }
    uint32_t offset() const { return offset_; }     // where data() starts in the image
    bool failed() const { return failed_; }

    // Ask for the chunk after the current one, if there is one and it has not
    // been asked for yet. Does not wait.
    void prefetch() {
        if (!pipelined_ || asked_ || failed_) return;
        uint32_t at = offset_ + (uint32_t)length_;
        if (at >= total_) return;
        link_.request(at, wantAt(at));
        asked_ = true;
    }

    // Move on to the next chunk. False at the end of the image (failed() says
    // whether it ended because a chunk could not be fetched).
    bool next() {
        uint32_t at = offset_ + (uint32_t)length_;
        if (failed_ || at >= total_) return false;
        size_t want = wantAt(at);
        bool ok = asked_ && link_.receive(at, nxt_, want);
        asked_ = false;
        if (!ok) ok = link_.fetch(at, nxt_, want);   // not asked, or the answer did not come: ask again
        if (!ok) { failed_ = true; return false; }
        uint8_t* done = cur_;
        cur_ = nxt_;
        nxt_ = done;
        offset_ = at;
        length_ = want;
        return true;
    }

private:
    size_t wantAt(uint32_t at) const {
        size_t left = total_ - at;
        return left < chunk_ ? left : chunk_;
    }

    Link& link_;
    uint8_t* cur_;
    uint8_t* nxt_;
    size_t chunk_;
    uint32_t total_;
    bool pipelined_;
    uint32_t offset_ = 0;
    size_t length_ = 0;
    bool asked_ = false;
    bool failed_ = false;
};

}  // namespace pulleys
