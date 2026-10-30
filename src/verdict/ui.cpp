#include "ui.h"
#include <TFT_eSPI.h>

static TFT_eSPI tft;
static TFT_eSprite btnSpr(&tft);
static TFT_eSprite statusSpr(&tft);

static const int W = 320, H = 240, HEADER_H = 28;

// Cold, clinical palette.
static uint16_t C_BG, C_INK, C_INK_SOFT, C_HEADER, C_HEADER_TXT, C_LINE,
    C_ACCENT, C_ACCENT_HI, C_ONLINE, C_BLUE, C_GREY, C_AMBER, C_RED;

static const char *HEADER_TITLE = "EVALUATION SYSTEM v2.6";

static uint16_t blend(uint16_t a, uint16_t b, float t) {
  uint8_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  uint8_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  return ((uint16_t)(ar + (br - ar) * t) << 11) | ((uint16_t)(ag + (bg - ag) * t) << 5) |
         (uint16_t)(ab + (bb - ab) * t);
}

static void drawHeader(uint16_t bar, const char *right) {
  tft.fillRect(0, 0, W, HEADER_H, bar);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(C_HEADER_TXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(HEADER_TITLE, 10, HEADER_H / 2);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(right, W - 10, HEADER_H / 2);
  if (bar == C_HEADER) {
    int tw = tft.textWidth(right);
    tft.fillCircle(W - 10 - tw - 8, HEADER_H / 2, 3, C_ONLINE);
  }
}

static void clearBody() { tft.fillRect(0, HEADER_H, W, H - HEADER_H, C_BG); }

void uiBegin() {
  tft.init();
  tft.setRotation(1);  // landscape, 320x240
  C_BG = tft.color565(244, 245, 247);
  C_INK = tft.color565(24, 28, 36);
  C_INK_SOFT = tft.color565(110, 116, 128);
  C_HEADER = tft.color565(24, 28, 36);
  C_HEADER_TXT = tft.color565(236, 238, 242);
  C_LINE = tft.color565(205, 210, 218);
  C_ACCENT = tft.color565(30, 41, 59);
  C_ACCENT_HI = tft.color565(82, 98, 122);
  C_ONLINE = tft.color565(45, 190, 150);
  C_BLUE = tft.color565(37, 99, 235);
  C_GREY = tft.color565(128, 132, 140);
  C_AMBER = tft.color565(217, 119, 6);
  C_RED = tft.color565(205, 28, 28);
  tft.fillScreen(C_BG);

  btnSpr.setColorDepth(16);
  btnSpr.createSprite(180, 56);
  statusSpr.setColorDepth(16);
  statusSpr.createSprite(260, 22);
}

// ---------------------------------------------------------------- idle

static const int BTN_X = (W - 180) / 2, BTN_Y = 118;
static uint32_t lastPulse = 0;

void uiIdleEnter() {
  drawHeader(C_HEADER, "ONLINE");
  clearBody();
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(C_INK);
  tft.drawString("Awaiting submission", W / 2, 78);
  tft.drawFastHLine(W / 2 - 40, 98, 80, C_LINE);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_INK_SOFT);
  tft.drawString("All evaluations are final.", W / 2, 212);
  lastPulse = 0;
}

void uiIdleTick(uint32_t now) {
  if (now - lastPulse < 40) return;
  lastPulse = now;
  float t = (sinf(now / 650.0f) + 1.0f) * 0.5f;
  btnSpr.fillSprite(C_BG);
  btnSpr.fillRoundRect(0, 0, 180, 56, 8, blend(C_ACCENT, C_ACCENT_HI, t));
  btnSpr.setFreeFont(&FreeSansBold12pt7b);
  btnSpr.setTextColor(C_HEADER_TXT);
  btnSpr.setTextDatum(MC_DATUM);
  btnSpr.drawString("SUBMIT", 90, 28);
  btnSpr.pushSprite(BTN_X, BTN_Y);
}

// ---------------------------------------------------------------- submitting

static const char *SUBTASKS[] = {
    "Tokenizing intent",          "Normalizing aesthetic vectors",
    "Cross-referencing precedent", "Calibrating taste",
    "Consulting the committee",   "Measuring originality",
    "Estimating audience patience", "Detecting pretension",
    "Weighing sincerity",         "Compiling feedback",
};
static const int SUBTASK_COUNT = sizeof(SUBTASKS) / sizeof(SUBTASKS[0]);

static const int BAR_X = 30, BAR_Y = 108, BAR_W = 260, BAR_H = 14;
static const int MAX_WP = 12;
static uint32_t wpTime[MAX_WP];
static uint8_t wpPct[MAX_WP];
static int wpCount;
static uint32_t submitDur;
static int barFilled;  // pixels
static int subtaskIdx, shownPct;
static uint32_t nextSubtaskAt;

static void sortU32(uint32_t *a, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && a[j - 1] > a[j]; j--) { uint32_t t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
}

static void drawStatus() {
  statusSpr.fillSprite(C_BG);
  statusSpr.setFreeFont(&FreeSans9pt7b);
  statusSpr.setTextColor(C_INK_SOFT);
  statusSpr.setTextDatum(ML_DATUM);
  char buf[48];
  snprintf(buf, sizeof buf, "%s...", SUBTASKS[subtaskIdx]);
  statusSpr.drawString(buf, 0, 11);
  statusSpr.setTextDatum(MR_DATUM);
  statusSpr.setTextColor(C_INK);
  snprintf(buf, sizeof buf, "%d%%", shownPct);
  statusSpr.drawString(buf, 260, 11);
  statusSpr.pushSprite(BAR_X, BAR_Y + BAR_H + 8);
}

void uiSubmitEnter(uint32_t durationMs) {
  submitDur = durationMs;
  // Stuttery progress: random jumps at random moments, landing on 100% at the end.
  int n = 5 + esp_random() % 5;
  uint32_t times[MAX_WP], pcts[MAX_WP];
  for (int i = 0; i < n; i++) {
    times[i] = 150 + esp_random() % (uint32_t)(durationMs * 0.9f);
    pcts[i] = 2 + esp_random() % 96;
  }
  sortU32(times, n);
  sortU32(pcts, n);
  for (int i = 0; i < n; i++) { wpTime[i] = times[i]; wpPct[i] = pcts[i]; }
  wpTime[n] = durationMs;
  wpPct[n] = 100;
  wpCount = n + 1;

  barFilled = 0;
  shownPct = 0;
  subtaskIdx = esp_random() % SUBTASK_COUNT;
  nextSubtaskAt = 600 + esp_random() % 500;

  drawHeader(C_HEADER, "PROCESSING");
  clearBody();
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(C_INK);
  tft.drawString("Submitting parameters...", W / 2, 76);
  tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, C_LINE);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_INK_SOFT);
  tft.drawString("Please do not leave the evaluation area.", W / 2, 205);
  drawStatus();
}

bool uiSubmitTick(uint32_t elapsed) {
  int pct = 0;
  for (int i = 0; i < wpCount; i++)
    if (elapsed >= wpTime[i]) pct = wpPct[i];

  bool dirty = false;
  int target = BAR_W * pct / 100;
  if (target > barFilled) {
    tft.fillRect(BAR_X + barFilled, BAR_Y, target - barFilled, BAR_H, C_ACCENT);
    barFilled = target;
  }
  if (pct != shownPct) { shownPct = pct; dirty = true; }
  if (elapsed >= nextSubtaskAt && pct < 100) {
    subtaskIdx = (subtaskIdx + 1 + esp_random() % (SUBTASK_COUNT - 1)) % SUBTASK_COUNT;
    nextSubtaskAt = elapsed + 550 + esp_random() % 600;
    dirty = true;
  }
  if (dirty) drawStatus();
  return elapsed >= submitDur;
}

// ---------------------------------------------------------------- verdict

static const int MAX_LINES = 4;
static String lines[MAX_LINES];
static int lineCount;

// Greedy word-wrap with the current font. Returns false if it doesn't fit.
static bool wrapText(const String &text, int maxW, int maxLines) {
  lineCount = 0;
  String cur;
  int start = 0;
  while (start <= (int)text.length()) {
    int sp = text.indexOf(' ', start);
    if (sp < 0) sp = text.length();
    String word = text.substring(start, sp);
    start = sp + 1;
    if (word.length() == 0) continue;
    if (tft.textWidth(word) > maxW) return false;
    String trial = cur.length() ? cur + " " + word : word;
    if (tft.textWidth(trial) <= maxW) {
      cur = trial;
    } else {
      if (lineCount >= maxLines) return false;
      lines[lineCount++] = cur;
      cur = word;
    }
  }
  if (cur.length()) {
    if (lineCount >= maxLines) return false;
    lines[lineCount++] = cur;
  }
  return true;
}

// Picks the biggest font that fits, lays out lines centered in the body.
// Returns the y of the bottom of the text block.
static int layoutVerdict(const String &text, bool bold) {
  const GFXfont *fonts[] = {bold ? &FreeSansBold24pt7b : &FreeSans24pt7b,
                            bold ? &FreeSansBold18pt7b : &FreeSans18pt7b,
                            bold ? &FreeSansBold12pt7b : &FreeSans12pt7b};
  for (int f = 0; f < 3; f++) {
    tft.setFreeFont(fonts[f]);
    if (wrapText(text, W - 30, f == 2 ? 4 : 3)) break;
  }
  return 0;
}

static void drawLines(int cx, int cy, uint16_t color) {
  int lh = tft.fontHeight();
  int top = cy - (lineCount * lh) / 2;
  tft.setTextColor(color);
  tft.setTextDatum(TC_DATUM);
  for (int i = 0; i < lineCount; i++) tft.drawString(lines[i], cx, top + i * lh);
}

static void drawWarning(int cx, int cy) {
  tft.fillTriangle(cx, cy - 14, cx - 16, cy + 13, cx + 16, cy + 13, C_AMBER);
  tft.fillRect(cx - 2, cy - 5, 4, 10, C_BG);
  tft.fillRect(cx - 2, cy + 7, 4, 4, C_BG);
}

void uiVerdict(const Verdict &v) {
  String text = v.text;
  const int cx = W / 2;
  int bodyMid = HEADER_H + (H - HEADER_H) / 2;

  if (v.tier == WORST) {
    text.toUpperCase();
    for (int i = 0; i < 3; i++) {
      tft.fillScreen(C_RED);
      delay(90);
      tft.fillScreen(C_BG);
      delay(70);
    }
    drawHeader(C_RED, "ALERT");
    layoutVerdict(text, true);
    int lh = tft.fontHeight();
    int blockH = lineCount * lh;
    // a few jittery frames before it settles
    for (int i = 0; i < 6; i++) {
      int dx = (int)(esp_random() % 9) - 4, dy = (int)(esp_random() % 7) - 3;
      tft.fillRect(0, HEADER_H, W, H - HEADER_H, C_BG);
      drawLines(cx + dx, bodyMid + dy, i % 2 ? C_INK : C_RED);
      delay(45);
    }
    tft.fillRect(0, HEADER_H, W, H - HEADER_H, C_BG);
    drawLines(cx, bodyMid, C_RED);
    int y = bodyMid + blockH / 2 + 10;
    tft.fillRect(cx - 50, y, 100, 4, C_RED);
    return;
  }

  drawHeader(C_HEADER, "COMPLETE");
  clearBody();
  layoutVerdict(text, false);
  int lh = tft.fontHeight();
  int blockH = lineCount * lh;

  uint16_t textColor = C_INK, rule = C_BLUE;
  int textMid = bodyMid;
  if (v.tier == MIDDLING) {
    textColor = C_GREY;
    rule = C_LINE;
  } else if (v.tier == NEEDS_WORK) {
    rule = C_AMBER;
    textMid = bodyMid + 14;
    drawWarning(cx, textMid - blockH / 2 - 24);
  }
  drawLines(cx, textMid, textColor);
  int y = textMid + blockH / 2 + 10;
  tft.fillRect(cx - 30, y, 60, 3, rule);
}

// ---------------------------------------------------------------- OTA

static int otaShown = -1;

void uiOtaProgress(int pct) {
  if (otaShown < 0) {
    drawHeader(C_HEADER, "MAINTENANCE");
    clearBody();
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSans12pt7b);
    tft.setTextColor(C_INK);
    tft.drawString("Updating firmware...", W / 2, 76);
    tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, C_LINE);
    otaShown = 0;
  }
  if (pct == otaShown) return;
  int from = BAR_W * otaShown / 100, to = BAR_W * pct / 100;
  if (to > from) tft.fillRect(BAR_X + from, BAR_Y, to - from, BAR_H, C_ACCENT);
  otaShown = pct;
}

void uiOtaError() {
  otaShown = -1;
  clearBody();
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(C_RED);
  tft.drawString("Update failed", W / 2, 120);
}
