#pragma once
// ── Text cards — words on a solid colour, a channel at a time ─────────────────
// The television (src/television) and the arbiter's queue sign
// (src/arbiter_mesh/signui) both show this: a table of cards, each on a named
// channel, played a channel at a time in table order and round again, fading
// through black between them. Text is wrapped and centred, at the largest
// size that fits.
//
//   pulleys::CardPlayer player(sprite, CARDS, count);  // sprite: full screen, PSRAM
//   player.tune(pulleys::settings_hash("wait"), millis());
//   player.tick(millis());                            // every loop; draws when needed
//
// A channel is named by the hash of its name (pulleys_settings), the way a
// setting's value arrives. CARD_DARK (0) is no channel: the screen black.
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <pulleys_settings.h>

namespace pulleys {

struct TextCard {
    const char* channel;   // lowercase word
    const char* text;      // "\n" forces a line break
    uint32_t    bg;        // 0xRRGGBB
    uint32_t    fg;
    uint16_t    seconds;   // how long it holds before the next on its channel
};

static constexpr uint16_t CARD_DARK = 0;

class CardPlayer {
public:
    static constexpr int     MARGIN    = 48;   // px kept clear on every side
    static constexpr uint8_t MAX_LINES = 12;
    static constexpr uint8_t LINE_LEN  = 128;

    CardPlayer(LGFX_Sprite& frame, const TextCard* cards, uint8_t count, uint16_t fadeMs = 350)
        : _f(frame), _cards(cards), _n(count), _fadeMs(fadeMs) {}

    // Called with true as the screen goes black for CARD_DARK, false as it
    // lights again: for a backlight.
    void onDark(void (*cb)(bool dark)) { _onDark = cb; }

    static uint16_t hashOf(const char* name) { return settings_hash(name); }
    uint16_t channelOf(uint8_t i) const { return hashOf(_cards[i].channel); }

    bool hasChannel(uint16_t ch) const {
        for (uint8_t i = 0; i < _n; i++) if (channelOf(i) == ch) return true;
        return false;
    }
    const char* channelName(uint16_t ch, const char* dark = "dark") const {
        if (ch == CARD_DARK) return dark;
        for (uint8_t i = 0; i < _n; i++) if (channelOf(i) == ch) return _cards[i].channel;
        return "?";
    }
    // Each channel once, in table order.
    void printChannels(Print& out) const {
        for (uint8_t i = 0; i < _n; i++) {
            bool first = true;
            for (uint8_t j = 0; j < i; j++) if (channelOf(j) == channelOf(i)) first = false;
            if (first) out.printf(" %s", _cards[i].channel);
        }
    }

    uint16_t channel() const { return _channel; }
    int8_t   current() const { return _current; }
    bool     held() const    { return _held; }
    void     hold(bool h, uint32_t now) {
        if (_held && !h && _phase == SHOW) _phaseAt = now;   // a full hold from here
        _held = h;
    }

    // Switch channel: fades straight to its first card. A channel with no
    // cards goes dark.
    void tune(uint16_t ch, uint32_t now) {
        _channel = ch;
        changeTo(stepOn(ch, -1, 1), now);
    }

    // Next (+1) or previous (-1) card on this channel, now. False if dark.
    bool step(int8_t dir, uint32_t now) {
        int8_t i = stepOn(_channel, _current, dir);
        if (i < 0) return false;
        changeTo(i, now);
        return true;
    }

    // Draw the current frame again on the next tick: for when something else
    // has had the screen.
    void redraw() { _lastLevel = 0xFFFF; _dirty = true; }

    void tick(uint32_t now) {
        if (_phase == DARK) {
            if (_dirty) { _f.fillScreen(0); _f.pushSprite(0, 0); _dirty = false; }
            return;
        }
        const TextCard& m = _cards[_current];
        uint32_t t = now - _phaseAt;
        uint16_t level = 256;
        switch (_phase) {
            case SHOW: {
                int8_t next = stepOn(_channel, _current, 1);
                if (!_held && next != _current && t >= (uint32_t)m.seconds * 1000)
                    changeTo(next, now);
                break;
            }
            case FADE_OUT:
                if (t >= _fadeMs) { show(_nextIdx, now); return; }
                level = 256 - t * 256 / _fadeMs;
                break;
            case FADE_IN:
                if (t >= _fadeMs) { _phase = SHOW; _phaseAt = now; break; }
                level = t * 256 / _fadeMs;
                break;
            default:
                break;
        }
        if (level != _lastLevel || _dirty) {
            draw(m, level);
            _lastLevel = level;
            _dirty = false;
        }
    }

private:
    enum Phase : uint8_t { SHOW, FADE_OUT, FADE_IN, DARK };

    LGFX_Sprite&    _f;
    const TextCard* _cards;
    uint8_t         _n;
    uint16_t        _fadeMs;
    void          (*_onDark)(bool) = nullptr;

    uint16_t _channel   = CARD_DARK;
    int8_t   _current   = -1;     // card on screen; -1 = dark
    int8_t   _nextIdx   = -1;     // where a fade out lands
    Phase    _phase     = DARK;
    uint32_t _phaseAt   = 0;
    bool     _held      = false;
    bool     _dirty     = true;
    uint16_t _lastLevel = 0xFFFF;

    char                _lines[MAX_LINES][LINE_LEN];
    uint8_t             _lineCount = 0;
    const lgfx::IFont*  _font = nullptr;

    // The card after `from` on channel `ch`, wrapping; -1 if it has none.
    // from = -1 starts at the channel's first (or last, walking back).
    int8_t stepOn(uint16_t ch, int8_t from, int8_t dir) const {
        if (ch == CARD_DARK) return -1;
        for (uint8_t k = 1; k <= _n; k++) {
            int i = from < 0 ? (dir > 0 ? k - 1 : _n - k)
                             : (from + dir * (int)k + _n * k) % _n;
            if (channelOf(i) == ch) return (int8_t)i;
        }
        return -1;
    }

    // Fade out of whatever is on, then into card i (or dark). From dark there
    // is nothing to fade out of.
    void changeTo(int8_t i, uint32_t now) {
        if (_current < 0) { if (i >= 0) show(i, now); return; }
        _nextIdx = i;
        if (_phase == FADE_OUT) return;          // already on the way down
        // Fading in? Start down from the level it has reached.
        uint32_t t = now - _phaseAt;
        _phaseAt = (_phase == FADE_IN && t < _fadeMs) ? now - (_fadeMs - t) : now;
        _phase   = _fadeMs ? FADE_OUT : _phase;
        if (!_fadeMs) show(i, now);
    }

    void show(int8_t i, uint32_t now) {
        if (i < 0) {
            _current = -1;
            _phase   = DARK;
            _dirty   = true;
            if (_onDark) _onDark(true);
            return;
        }
        if (_current < 0 && _onDark) _onDark(false);
        _current = i;
        layout(_cards[i]);
        Serial.printf("  [CARD] %s: \"%s\" (%u lines)\n", _cards[i].channel, _cards[i].text, _lineCount);
        _phase     = _fadeMs ? FADE_IN : SHOW;
        _phaseAt   = now;
        _lastLevel = 0xFFFF;
    }

    // ── Layout ────────────────────────────────────────────────────────────────
    // Greedy word wrap at the current font. False if it does not fit: too many
    // lines, too tall, or one word wider than the screen on its own.
    bool wrap(const char* text) {
        const int maxW = _f.width() - 2 * MARGIN;
        _lineCount = 0;
        char cur[LINE_LEN] = "";
        const char* p = text;
        while (true) {
            while (*p == ' ') p++;
            const char* w = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            size_t wl = p - w;
            if (wl > 0) {
                char trial[LINE_LEN];
                size_t cl = strlen(cur);
                if (cl + 1 + wl >= LINE_LEN) return false;
                snprintf(trial, sizeof(trial), cl ? "%s %.*s" : "%s%.*s", cur, (int)wl, w);
                if (_f.textWidth(trial) <= maxW) {
                    strcpy(cur, trial);
                } else {
                    if (cl == 0) return false;                 // the word alone is too wide
                    if (_lineCount >= MAX_LINES) return false;
                    strcpy(_lines[_lineCount++], cur);
                    snprintf(cur, sizeof(cur), "%.*s", (int)wl, w);
                    if (_f.textWidth(cur) > maxW) return false;
                }
            }
            if (*p == '\n' || *p == 0) {
                if (_lineCount >= MAX_LINES) return false;
                strcpy(_lines[_lineCount++], cur);
                cur[0] = 0;
                if (*p == 0) break;
                p++;
            }
        }
        return _lineCount * _f.fontHeight() <= _f.height() - 2 * MARGIN;
    }

    void layout(const TextCard& m) {
        // Largest first; the first that fits the card wins.
        static const lgfx::IFont* const FONTS[] = {
            &fonts::DejaVu72, &fonts::DejaVu56, &fonts::DejaVu40,
            &fonts::DejaVu24, &fonts::DejaVu18, &fonts::DejaVu12,
        };
        for (const lgfx::IFont* f : FONTS) {
            _font = f;
            _f.setFont(f);
            if (wrap(m.text)) return;
        }
        Serial.printf("  [CARD] \"%s\" does not fit even at the smallest size: clipped\n", m.text);
    }

    // ── Drawing ───────────────────────────────────────────────────────────────
    static uint16_t dim565(uint32_t rgb, uint16_t level) {   // level 0..256
        uint8_t r = ((rgb >> 16) & 0xFF) * level >> 8;
        uint8_t g = ((rgb >> 8)  & 0xFF) * level >> 8;
        uint8_t b = ( rgb        & 0xFF) * level >> 8;
        return lgfx::color565(r, g, b);
    }

    void draw(const TextCard& m, uint16_t level) {
        uint16_t bg = dim565(m.bg, level);
        _f.fillScreen(bg);
        _f.setFont(_font);
        _f.setTextColor(dim565(m.fg, level), bg);
        _f.setTextDatum(lgfx::top_center);
        int lh = _f.fontHeight();
        int y  = (_f.height() - _lineCount * lh) / 2;
        for (uint8_t i = 0; i < _lineCount; i++, y += lh)
            _f.drawString(_lines[i], _f.width() / 2, y);
        _f.pushSprite(0, 0);
    }
};

}  // namespace pulleys
