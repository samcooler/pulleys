# CYD Verdict Machine

An art piece. You press a button, a cold, corporate "AI evaluation system" pretends to
analyze your submission, then hands down a verdict.

## Flashing
- First time / recovery over USB: `pio run -e verdict -t upload`
- Over WiFi: `pio run -e verdict_ota -t upload` (host + password come from `src/verdict/secrets.h`)
- Credentials: copy `src/verdict/secrets.h.example` to `src/verdict/secrets.h` (gitignored).
- Hostname `verdict-machine`; the serial log prints its IP on connect.
- Edit the verdict list in `src/verdict/verdicts.h`.

## Spec (v1)

### Hardware
- Cheap Yellow Display (ESP32-2432S028R family, 320x240 TFT with resistive touch).
  Single micro-USB variant (CH340), ESP32-D0WD-V3, 4MB flash.
- WiFi + ArduinoOTA, so after the first USB flash it updates over the air.
- Touch: a tap anywhere on the screen counts as SUBMIT (no touch calibration needed).
- External momentary button on IO27 (CN1 header), wired to GND, INPUT_PULLUP plus software debounce.
  The CYD has no onboard pull resistors on the header pins. IO22/IO27 have internal pulls, but IO35 is input-only with NO internal pull, so it would need an external 10k to 3.3V. Avoid IO21 (backlight on some variants). It triggers the same code path as the touch button.
- Not used in v1: the speaker and the RGB LED.

### States
1. IDLE / ATTRACT
   - Clean light UI: off-white background, thin header bar "EVALUATION SYSTEM v2.6",
     small status text "Awaiting submission", and a slowly pulsing SUBMIT button.
   - Touching SUBMIT (or pressing the external button) goes to SUBMITTING.
2. SUBMITTING
   - Text: "Submitting parameters..."
   - In-progress indicator: a horizontal progress bar that fills in uneven, stuttery steps
     (it feels like real work), plus a small rotating subtask line ("Tokenizing intent",
     "Cross-referencing precedent", "Calibrating taste", ...).
   - Lasts a random 3-6 seconds. Input is ignored during this state.
   - Then goes to VERDICT.
3. VERDICT
   - Shows ONLY the verdict text, large and centered, word-wrapped to fit.
     No tier name and no score.
   - Styling depends on the tier (see below).
   - Pressing again starts a new SUBMITTING right away.
   - After ~20 s with no input, goes back to IDLE.

### Randomness
- Every verdict has equal odds (1/33). Tiers with more lines come up more often:
  Almost 12/33, Needs work 10/33, Middling 6/33, Worst 5/33.
- The same verdict never shows twice in a row (re-roll on a repeat).
- Seeded from esp_random() (hardware RNG).

### Tier styling (cold/clinical, escalating)
| Tier       | Accent        | Treatment                                                     |
|------------|---------------|---------------------------------------------------------------|
| Almost     | calm blue     | plain verdict, thin blue rule underneath                      |
| Middling   | neutral grey  | plain verdict, muted grey text                                |
| Needs work | amber         | amber rule plus a small warning glyph                         |
| Worst      | red           | screen flashes red/inverted 2-3x, verdict in bold red caps, slight jitter on entry |

### Verdict table (one editable array in verdicts.h: {tier, text})
Almost: Edgy... maybe too edgy / Too Verbose / Requires further testing / Missing something /
  Make me care / Needs a redraft / Kick it up a notch / Keep trying / Almost something /
  Close but no cigar / Get to the point / I want to believe
Middling: 404: OUTPUT NOT FOUND / ...meh / Mediocre / So last year / Underdeveloped / Feels hollow
Needs work: Derivative / Concerning / Exercise caution / Cliche / Passe / Graphic content alert /
  Conceited / Naive / Lost the thread / Snoozefest
Worst: INCONCEIVABLE / Slop / Unwatchable / Hot mess / Trash

Note: the "…" ellipsis and accented letters (cliché, passé) get drawn as ASCII
("...", "Cliche", "Passe") because the built-in fonts don't include those characters.
Upgradeable later with a custom font.

### Software
- PlatformIO env `verdict`: board esp32dev, framework arduino (pioarduino, Arduino core 3.3).
- Libraries: TFT_eSPI (display, configured with build_flags, no editing of User_Setup.h) and
  XPT2046_Touchscreen (touch on its own SPI bus: IO25/32/39/33/36).
- Drawing uses a TFT_eSprite for flicker-free animation where needed.
- Code layout: main.cpp (state machine), ui.cpp/.h (screens), verdicts.h (the table),
  input.cpp/.h (touch + IO27 button, one shared "pressed" event), net.cpp/.h (WiFi + ArduinoOTA).
- Lives in this repo as `src/verdict/` with envs `verdict` (USB) and `verdict_ota` (WiFi).

### Done when
- It flashes (USB or OTA) and boots into IDLE.
- Tap, then 3-6 s of submitting, then a verdict styled by tier, then back to IDLE after ~20 s.
- Over ~50 presses: no immediate repeats, and all four tiers show up.

### Stretch (not v1)
- Speaker blips/buzzer on Worst, RGB LED glows the tier color,
  custom smooth font, landscape/portrait choice, a slowly growing "submissions evaluated"
  counter on the idle screen.
