#pragma once
#include <Arduino.h>

// External button on IO27, wired to GND (INPUT_PULLUP, active LOW).
#define BUTTON_PIN 27

void inputBegin();
// Returns true exactly once per press, from either the touchscreen or the button.
bool inputPressed();
