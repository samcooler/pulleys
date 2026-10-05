#pragma once

#include <stdint.h>
#include <FastLED.h>

// ── pulleys_panel — where a shape's pixels actually are on the strip ──────────
//
// Every LED surface in the piece is one strip folded into rows, and a renderer
// should not have to know how it was folded. A Panel is that knowledge and
// nothing else: given logical (x, y) it answers with an index into the strip.
// Renderers work in (x, y); only the Panel knows about serpentine runs, dead
// pixels at a fold, or how far down the strip this surface starts.
//
//   x  0..cols-1   along the strip's run
//   y  0..rows-1   which run — row 0 is the one nearest `origin`
//
// The two surfaces the piece uses:
//
//   8×8    64 LEDs, the block a Screen shows one channel in, and the panel on
//          a Sensor. cols=8 rows=8 gap=0 serpentine.
//   16×2   33 LEDs: 16 out, one dead pixel at the fold, 16 back.
//          cols=16 rows=2 gap=1 serpentine — see panel_16x2().
//
// `origin` is what makes them stack: a Sensor carrying an 8×8 with a 16×2
// chained after it is panel_8x8(leds, 0) and panel_16x2(leds, 64) over one
// array, with no renderer aware of the other's existence.
//
// `swap` is there because two surfaces on one chain can want different channel
// orders, and a FastLED controller emits exactly one order for the whole chain.
// Adding a second controller on the same pin does not help: it latches between
// the two, and a WS2812 chain reads anything after a latch as a new frame, so
// the second surface's data would land on the first surface's pixels. The
// surface that disagrees therefore has its channels permuted in the buffer,
// which is this -- see panel_fix_order().

namespace pulleys {

// Which two channels this surface's chip has exchanged, relative to the order
// the chain's controller was told. SWAP_NONE is a surface that agrees with it.
enum ChannelSwap : uint8_t {
    SWAP_NONE = 0,
    SWAP_RB,          // red and blue exchanged — e.g. a GBR panel on a GRB chain
    SWAP_RG,          // red and green exchanged
    SWAP_GB,          // green and blue exchanged
};

struct Panel {
    CRGB*       strip      = nullptr;  // the LED array this surface lives in
    uint16_t    origin     = 0;        // strip index of logical (0, 0)
    uint8_t     cols       = 8;        // pixels per run
    uint8_t     rows       = 8;        // number of runs
    uint8_t     gap        = 0;        // dead pixels between one run and the next
    bool        serpentine = true;     // every other run is wired backwards
    ChannelSwap swap       = SWAP_NONE;// this chip's order vs the chain's
};

// Strip pixels this panel occupies, dead ones included. The next panel chained
// after it starts at origin + panel_leds().
inline uint16_t panel_leds(const Panel& p) {
    return (uint16_t)p.rows * p.cols + (uint16_t)(p.rows - 1) * p.gap;
}

// Strip index of logical (x, y). Out-of-range coordinates are the caller's bug,
// so this does not clamp -- a renderer that walks past its own panel should
// look wrong on the bench rather than quietly fold back onto itself.
inline uint16_t panel_index(const Panel& p, uint8_t x, uint8_t y) {
    uint8_t run = (p.serpentine && (y & 1)) ? (uint8_t)(p.cols - 1 - x) : x;
    return p.origin + (uint16_t)y * (p.cols + p.gap) + run;
}

// Write one logical pixel.
inline void panel_set(const Panel& p, uint8_t x, uint8_t y, const CRGB& c) {
    p.strip[panel_index(p, x, y)] = c;
}

// ── The two surfaces ──────────────────────────────────────────────────────────

inline Panel panel_8x8(CRGB* strip, uint16_t origin = 0,
                       bool serpentine = true, ChannelSwap swap = SWAP_NONE) {
    Panel p;
    p.strip = strip; p.origin = origin;
    p.cols = 8; p.rows = 8; p.gap = 0; p.serpentine = serpentine;
    p.swap = swap;
    return p;
}

// 16 out, skip 1, 16 back. The skipped pixel is the one at the fold: it exists
// on the strip and is never written, so it stays dark and the two runs line up
// column for column. That is why `gap` is part of the geometry rather than
// something a caller subtracts -- get it wrong and row 1 is off by one for its
// whole length, which reads as a wiring fault.
inline Panel panel_16x2(CRGB* strip, uint16_t origin = 0,
                        bool serpentine = true, ChannelSwap swap = SWAP_NONE) {
    Panel p;
    p.strip = strip; p.origin = origin;
    p.cols = 16; p.rows = 2; p.gap = 1; p.serpentine = serpentine;
    p.swap = swap;
    return p;
}

// Permute this surface's pixels in place so a chip that disagrees with the
// chain's declared order still shows the colour that was asked for.
//
// Call it on the whole surface after everything has drawn and immediately
// before FastLED.show(), never from a renderer: it is an output-stage fixup,
// and a pattern that applied it would be undone by the next pattern to write
// the same pixel -- or applied twice, which cancels out and looks like nothing
// happened. Dead pixels in the span are included; permuting black costs
// nothing and skipping them would need the geometry walked for no reason.
inline void panel_fix_order(const Panel& p) {
    if (p.swap == SWAP_NONE) return;
    const uint16_t n = panel_leds(p);
    for (uint16_t i = 0; i < n; i++) {
        CRGB& c = p.strip[p.origin + i];
        switch (p.swap) {
            case SWAP_RB: { uint8_t t = c.r; c.r = c.b; c.b = t; break; }
            case SWAP_RG: { uint8_t t = c.r; c.r = c.g; c.g = t; break; }
            case SWAP_GB: { uint8_t t = c.g; c.g = c.b; c.b = t; break; }
            default: break;
        }
    }
}

static constexpr uint16_t PANEL_8X8_LEDS  = 64;
static constexpr uint16_t PANEL_16X2_LEDS = 33;

}  // namespace pulleys
