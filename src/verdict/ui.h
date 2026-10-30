#pragma once
#include <Arduino.h>
#include "verdicts.h"

void uiBegin();

void uiIdleEnter();
void uiIdleTick(uint32_t now);

void uiSubmitEnter(uint32_t durationMs);
// Returns true once the submission animation has finished.
bool uiSubmitTick(uint32_t elapsedMs);

void uiVerdict(const Verdict &v);

// Firmware update screen (OTA).
void uiOtaProgress(int pct);
void uiOtaError();
