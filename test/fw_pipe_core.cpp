// Host test for lib/pulleys_fw_serial/pulleys_fw_pipe.h: the order of requests and hand-offs.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "lib/pulleys_fw_serial/pulleys_fw_pipe.h"

struct FakeLink {
    std::vector<uint8_t> image;
    std::vector<std::string> log;
    std::set<uint32_t> receive_fails;     // offsets whose prefetched answer never comes
    std::set<uint32_t> fetch_fails;       // offsets that cannot be fetched at all
    const uint8_t* in_use = nullptr;      // the chunk the caller is handing on right now

    void request(uint32_t offset, size_t) { log.push_back("request@" + std::to_string(offset)); }
    bool receive(uint32_t offset, uint8_t* out, size_t want) {
        log.push_back("receive@" + std::to_string(offset));
        assert(out != in_use && "a received chunk must never land in the buffer being used");
        if (receive_fails.count(offset)) return false;
        std::memcpy(out, image.data() + offset, want);
        return true;
    }
    bool fetch(uint32_t offset, uint8_t* out, size_t want) {
        log.push_back("fetch@" + std::to_string(offset));
        assert(out != in_use && "a fetched chunk must never land in the buffer being used");
        if (fetch_fails.count(offset)) return false;
        std::memcpy(out, image.data() + offset, want);
        return true;
    }
};

typedef pulleys::ChunkPipe<FakeLink> Pipe;

static FakeLink make(size_t size) {
    FakeLink link;
    for (size_t i = 0; i < size; ++i) link.image.push_back((uint8_t)(i * 7 + 3));
    return link;
}

// Run a whole transfer the way the bridge does, logging "use@<offset>" for each hand-off.
static bool run(FakeLink& link, size_t size, bool pipelined, size_t chunk = 4) {
    uint8_t a[16], b[16];
    Pipe pipe(link, a, b, chunk, (uint32_t)size, pipelined);
    link.in_use = nullptr;
    if (!pipe.first()) return false;
    do {
        pipe.prefetch();
        link.in_use = pipe.data();
        assert(pipe.length() > 0 && pipe.length() <= chunk);
        assert(std::memcmp(pipe.data(), link.image.data() + pipe.offset(), pipe.length()) == 0 && "chunk bytes are the image's");
        link.log.push_back("use@" + std::to_string(pipe.offset()));
    } while (pipe.next());
    return !pipe.failed();
}

static std::string joined(const std::vector<std::string>& log) {
    std::string out;
    for (size_t i = 0; i < log.size(); ++i) out += (i ? " " : "") + log[i];
    return out;
}

int main() {
    // Pipelined: the next chunk is requested BEFORE the current one is used.
    {
        FakeLink link = make(10);
        assert(run(link, 10, true));
        assert(joined(link.log) == "fetch@0 request@4 use@0 receive@4 request@8 use@4 receive@8 use@8");
    }
    // Not pipelined (the BRIDGE_PIPELINE=0 fallback): strictly one at a time, never a prefetch.
    {
        FakeLink link = make(10);
        assert(run(link, 10, false));
        assert(joined(link.log) == "fetch@0 use@0 fetch@4 use@4 fetch@8 use@8");
    }
    // The answer to a prefetch does not come: ask again, and carry on.
    {
        FakeLink link = make(10);
        link.receive_fails.insert(4);
        assert(run(link, 10, true));
        assert(joined(link.log) == "fetch@0 request@4 use@0 receive@4 fetch@4 request@8 use@4 receive@8 use@8");
    }
    // A chunk that cannot be fetched at all ends the transfer as a failure, after the earlier chunks were used.
    {
        FakeLink link = make(10);
        link.fetch_fails.insert(8);
        link.receive_fails.insert(8);
        assert(!run(link, 10, true));
        assert(joined(link.log) == "fetch@0 request@4 use@0 receive@4 request@8 use@4 receive@8 fetch@8");
    }
    // The very first chunk failing: nothing is used.
    {
        FakeLink link = make(10);
        link.fetch_fails.insert(0);
        assert(!run(link, 10, true));
        assert(joined(link.log) == "fetch@0");
    }
    // An image that is exactly one chunk: no prefetch, and the end is not a failure.
    {
        FakeLink link = make(4);
        assert(run(link, 4, true));
        assert(joined(link.log) == "fetch@0 use@0");
    }
    // An exact multiple of the chunk size: nothing is requested past the end.
    {
        FakeLink link = make(8);
        assert(run(link, 8, true));
        assert(joined(link.log) == "fetch@0 request@4 use@0 receive@4 use@4");
    }
    // A short last chunk comes back at its true length.
    {
        FakeLink link = make(9);
        uint8_t a[16], b[16];
        Pipe pipe(link, a, b, 4, 9, true);
        assert(pipe.first()); assert(pipe.length() == 4);
        assert(pipe.next()); assert(pipe.length() == 4 && pipe.offset() == 4);
        assert(pipe.next()); assert(pipe.length() == 1 && pipe.offset() == 8);
        assert(!pipe.next() && !pipe.failed());
    }
    // Asking twice asks once; asking at the end asks for nothing.
    {
        FakeLink link = make(8);
        uint8_t a[16], b[16];
        Pipe pipe(link, a, b, 4, 8, true);
        assert(pipe.first());
        pipe.prefetch(); pipe.prefetch();
        assert(joined(link.log) == "fetch@0 request@4");
        assert(pipe.next());
        pipe.prefetch();
        assert(joined(link.log) == "fetch@0 request@4 receive@4");
    }
    // Moving on without having asked still works, by fetching.
    {
        FakeLink link = make(8);
        uint8_t a[16], b[16];
        Pipe pipe(link, a, b, 4, 8, true);
        assert(pipe.first());
        assert(pipe.next());
        assert(joined(link.log) == "fetch@0 fetch@4");
        assert(std::memcmp(pipe.data(), link.image.data() + 4, 4) == 0);
    }
    // An empty image has nothing to fetch, and that is not a failed fetch.
    {
        FakeLink link = make(0);
        uint8_t a[16], b[16];
        Pipe pipe(link, a, b, 4, 0, true);
        assert(!pipe.first() && !pipe.failed());
        assert(link.log.empty());
    }
    // A long image alternates the two buffers and every byte arrives intact.
    {
        FakeLink link = make(1000);
        uint8_t a[64], b[64];
        Pipe pipe(link, a, b, 64, 1000, true);
        uint32_t seen = 0;
        const uint8_t* last = nullptr;
        assert(pipe.first());
        do {
            pipe.prefetch();
            link.in_use = pipe.data();
            assert(pipe.data() != last && (pipe.data() == a || pipe.data() == b));
            assert(std::memcmp(pipe.data(), link.image.data() + pipe.offset(), pipe.length()) == 0);
            last = pipe.data();
            seen += (uint32_t)pipe.length();
        } while (pipe.next());
        assert(!pipe.failed() && seen == 1000);
    }
    return 0;
}
