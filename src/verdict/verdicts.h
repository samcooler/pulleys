#pragma once
#include <stddef.h>

// Edit this table to change the verdicts. Every entry has equal odds.
enum Tier { ALMOST, MIDDLING, NEEDS_WORK, WORST };

struct Verdict {
  Tier tier;
  const char *text;
};

static const Verdict VERDICTS[] = {
  // Almost
  {ALMOST, "Edgy... maybe too edgy"},
  {ALMOST, "Too Verbose"},
  {ALMOST, "Requires further testing"},
  {ALMOST, "Missing something"},
  {ALMOST, "Make me care"},
  {ALMOST, "Needs a redraft"},
  {ALMOST, "Kick it up a notch"},
  {ALMOST, "Keep trying"},
  {ALMOST, "Almost something"},
  {ALMOST, "Close but no cigar"},
  {ALMOST, "Get to the point"},
  {ALMOST, "I want to believe"},
  // Middling
  {MIDDLING, "404: OUTPUT NOT FOUND"},
  {MIDDLING, "...meh"},
  {MIDDLING, "Mediocre"},
  {MIDDLING, "So last year"},
  {MIDDLING, "Underdeveloped"},
  {MIDDLING, "Feels hollow"},
  // Needs work
  {NEEDS_WORK, "Derivative"},
  {NEEDS_WORK, "Concerning"},
  {NEEDS_WORK, "Exercise caution"},
  {NEEDS_WORK, "Cliche"},
  {NEEDS_WORK, "Passe"},
  {NEEDS_WORK, "Graphic content alert"},
  {NEEDS_WORK, "Conceited"},
  {NEEDS_WORK, "Naive"},
  {NEEDS_WORK, "Lost the thread"},
  {NEEDS_WORK, "Snoozefest"},
  // Worst
  {WORST, "INCONCEIVABLE"},
  {WORST, "Slop"},
  {WORST, "Unwatchable"},
  {WORST, "Hot mess"},
  {WORST, "Trash"},
};

static const size_t VERDICT_COUNT = sizeof(VERDICTS) / sizeof(VERDICTS[0]);
