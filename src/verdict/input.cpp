#include "input.h"
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// CYD touch controller lives on its own SPI bus.
#define XPT_IRQ 36
#define XPT_MOSI 32
#define XPT_MISO 39
#define XPT_CLK 25
#define XPT_CS 33

static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);

static const uint32_t DEBOUNCE_MS = 25;

static bool btnStable = HIGH, btnRaw = HIGH;
static uint32_t btnChanged = 0;

static bool touchStable = false, touchRaw = false;
static uint32_t touchChanged = 0;

void inputBegin() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  touchSpi.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);
}

bool inputPressed() {
  uint32_t now = millis();
  bool event = false;

  bool b = digitalRead(BUTTON_PIN);
  if (b != btnRaw) { btnRaw = b; btnChanged = now; }
  if (now - btnChanged >= DEBOUNCE_MS && btnRaw != btnStable) {
    btnStable = btnRaw;
    if (btnStable == LOW) event = true;
  }

  bool t = ts.touched();
  if (t != touchRaw) { touchRaw = t; touchChanged = now; }
  if (now - touchChanged >= 40 && touchRaw != touchStable) {
    touchStable = touchRaw;
    if (touchStable) event = true;
  }

  return event;
}
