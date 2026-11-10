#include "signui.h"
#include "devices.h"
#include "sign_messages.h"
#include <lvgl.h>
#include <pulleys_textcard.h>
#include <pulleys_settings.h>

#define SIGN_PREFIX  "  [SIGN]"
#define SIGN_SETTING "queue.sign"

// The secret order, as quadrants: 0 top left, 1 top right, 2 bottom left,
// 3 bottom right. Default: top left, bottom right, bottom left, top right.
// Build with -D 'ARBITER_SIGN_UNLOCK="0123"' for another.
#ifndef ARBITER_SIGN_UNLOCK
#define ARBITER_SIGN_UNLOCK "0321"
#endif
#ifndef ARBITER_SIGN_TAP_GAP_MS
#define ARBITER_SIGN_TAP_GAP_MS 3000         // longer between taps and it starts over
#endif
#ifndef ARBITER_SIGN_IDLE_MS
#define ARBITER_SIGN_IDLE_MS (5UL * 60 * 1000) // monitor untouched this long: sign again
#endif
#define SIGN_TOUCH_POLL_MS 20

static const char UNLOCK[] = ARBITER_SIGN_UNLOCK;
static constexpr uint8_t UNLOCK_LEN = sizeof(UNLOCK) - 1;
static_assert(UNLOCK_LEN >= 2, "ARBITER_SIGN_UNLOCK: at least two taps");

static lgfx::LovyanGFX*     s_display = nullptr;
static pulleys::GT911*      s_touch   = nullptr;
static LGFX_Sprite*         s_frame   = nullptr;
static pulleys::CardPlayer* s_player  = nullptr;
static lv_obj_t*            s_btn     = nullptr;

static bool     s_showing      = true;    // the sign, from boot, if the game has one
static bool     s_available    = false;   // this game has a queue.sign
static bool     s_toMonitor    = false;   // unlocked: hand over once the finger lifts
static bool     s_down         = false;
static uint8_t  s_progress     = 0;       // taps of UNLOCK matched so far
static uint32_t s_lastTap      = 0;
static uint32_t s_lastTouch    = 0;       // on the monitor
static uint32_t s_lastPoll     = 0;
static uint16_t s_tunedTo      = 0xFFFF;

static void setButtonVisible(bool on) {
    if (!s_btn) return;
    if (on) lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
}

void signui_show(bool sign) {
    if (sign && !s_available) {
        Serial.println(SIGN_PREFIX " this game has no " SIGN_SETTING ": staying on the monitor");
        sign = false;
    }
    if (sign == s_showing) return;
    s_showing   = sign;
    s_toMonitor = false;
    s_progress  = 0;
    if (sign) {
        if (s_player) s_player->redraw();
    } else {
        s_lastTouch = millis();
        lv_obj_invalidate(lv_scr_act());   // LVGL repaints the whole monitor over the sign
    }
    Serial.printf(SIGN_PREFIX " %s\n", sign ? "sign up" : "monitor up");
}

bool signui_showing() { return s_showing; }
void signui_note_touch() { s_lastTouch = millis(); }

static void signBtnCb(lv_event_t*) { signui_show(true); }

void signui_init(lgfx::LovyanGFX* display, pulleys::GT911* touch) {
    s_display = display;
    s_touch   = touch;

    s_frame = new LGFX_Sprite(display);
    s_frame->setColorDepth(16);
    s_frame->setPsram(true);
    if (!s_frame->createSprite(display->width(), display->height())) {
        Serial.println(SIGN_PREFIX " no PSRAM for the frame: the sign is off");
        delete s_frame;
        s_frame = nullptr;
        s_showing = false;
    } else {
        s_player = new pulleys::CardPlayer(*s_frame, SIGN_MESSAGES, SIGN_MESSAGE_COUNT);
    }

    // In the SCREENS quadrant's title row, where the install openers sit in theirs.
    s_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_btn, 118, 26);
    lv_obj_set_pos(s_btn, 800 - 124, 3);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0xB3261E), 0);
    lv_obj_set_style_radius(s_btn, 4, 0);
    lv_obj_add_event_cb(s_btn, signBtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(s_btn);
    lv_label_set_text(l, "SIGN");
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    setButtonVisible(false);
    s_lastTouch = millis();
}

// One tap on the sign, at (x, y): advance the secret, or start it over.
static void tap(uint16_t x, uint16_t y, uint32_t now) {
    char q = '0' + (x >= s_display->width() / 2) + 2 * (y >= s_display->height() / 2);
    if (s_progress && now - s_lastTap > ARBITER_SIGN_TAP_GAP_MS) s_progress = 0;
    s_lastTap = now;
    if (q == UNLOCK[s_progress])  s_progress++;
    else                          s_progress = (q == UNLOCK[0]) ? 1 : 0;
    if (s_progress == UNLOCK_LEN) {
        s_progress  = 0;
        s_toMonitor = true;      // the finger is still down: wait for it to lift
        Serial.println(SIGN_PREFIX " unlocked");
    }
}

bool signui_tick(uint32_t now, bool modalOpen) {
    // Which sign the game wants, if it has one at all.
    const char* want = devices_want(SIGN_SETTING);
    bool avail = want && s_player;
    if (avail != s_available) {
        s_available = avail;
        setButtonVisible(avail);
        if (!avail && s_showing) signui_show(false);
    }
    if (!s_available) return false;

    uint16_t ch = pulleys::settings_hash(want);
    if (ch != s_tunedTo) {
        s_tunedTo = ch;
        if (!s_player->hasChannel(ch))
            Serial.printf(SIGN_PREFIX " no cards for %s=%s: the sign is black\n", SIGN_SETTING, want);
        s_player->tune(ch, now);
    }
    devices_report_local(SIGN_SETTING, want);

    if (!s_showing) {
        if (!modalOpen && now - s_lastTouch >= ARBITER_SIGN_IDLE_MS) signui_show(true);
        return s_showing;
    }

    if (now - s_lastPoll >= SIGN_TOUCH_POLL_MS) {
        s_lastPoll = now;
        uint16_t x, y;
        bool down = s_touch && s_touch->readSticky(x, y);
        if (down && !s_down) tap(x, y, now);
        s_down = down;
        if (!down && s_toMonitor) { signui_show(false); return false; }
    }

    s_player->tick(now);
    return true;
}
