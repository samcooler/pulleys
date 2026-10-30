#include <Arduino.h>
#include "input.h"
#include "net.h"
#include "ui.h"
#include "verdicts.h"

enum State { IDLE, SUBMITTING, VERDICT };

static const uint32_t VERDICT_TIMEOUT_MS = 20000;
static const uint32_t SUBMIT_MIN_MS = 3000, SUBMIT_MAX_MS = 6000;

static State state = IDLE;
static uint32_t stateStart = 0;
static int lastVerdict = -1;

static int pickVerdict() {
  int i;
  do {
    i = esp_random() % VERDICT_COUNT;  // hardware RNG, equal odds per verdict
  } while (VERDICT_COUNT > 1 && i == lastVerdict);
  lastVerdict = i;
  return i;
}

static void startSubmit() {
  state = SUBMITTING;
  stateStart = millis();
  uint32_t dur = SUBMIT_MIN_MS + esp_random() % (SUBMIT_MAX_MS - SUBMIT_MIN_MS + 1);
  uiSubmitEnter(dur);
}

static void goIdle() {
  state = IDLE;
  stateStart = millis();
  uiIdleEnter();
}

void setup() {
  Serial.begin(115200);
  uiBegin();
  inputBegin();
  goIdle();
  netBegin();
  Serial.println("verdict machine ready");
}

void loop() {
  uint32_t now = millis();
  netTick();
  bool pressed = inputPressed();

  switch (state) {
    case IDLE:
      if (pressed) { startSubmit(); break; }
      uiIdleTick(now);
      break;

    case SUBMITTING:
      // presses are ignored while "evaluating"
      if (uiSubmitTick(now - stateStart)) {
        delay(250);
        int i = pickVerdict();
        Serial.printf("verdict #%d: %s\n", i, VERDICTS[i].text);
        state = VERDICT;
        uiVerdict(VERDICTS[i]);
        stateStart = millis();
        inputPressed();  // drop any press that landed during the animation
      }
      break;

    case VERDICT:
      if (pressed) { startSubmit(); break; }
      if (now - stateStart > VERDICT_TIMEOUT_MS) goIdle();
      break;
  }
  delay(5);
}
