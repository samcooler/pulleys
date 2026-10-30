#pragma once
#include <Arduino.h>

// Non-blocking WiFi + ArduinoOTA. Call netBegin() in setup, netTick() every loop.
void netBegin();
void netTick();
