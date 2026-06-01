# Battery Discharge Analysis — Traveler v1

**Conditions:** Sleep disabled (`DISABLE_SLEEP`), LEDs at 50% max brightness, BLE advertising + scanning active.

---

## Log contents

Three sessions captured in the 1024-sample rolling NVS log:

| Session | Duration | Notes |
|---------|----------|-------|
| 1 | ~1.5 min | Quick reboot, discarded |
| 2 | ~54 min | Partial — USB reconnected mid-run (voltage spike at 3243s) |
| 3 | **188 min** | Full discharge — analysis below |

---

## Session 3: Full discharge

| Metric | Value |
|--------|-------|
| Start voltage | 4.001 V (~90% charge, not fully topped) |
| End voltage | 2.641 V (protection cutoff) |
| Total runtime | **188 min (3h 8m)** |
| Low-battery warning fired | ~173 min (3.4V threshold) |
| Time from warning to death | ~15 min |

### Discharge curve shape

```
4.0V ┤▓ start
     │▓▓ rapid surface-charge drop (~first 30 min, −250mV)
3.8V ┤  ▓▓▓
     │     ▓▓▓▓▓▓ long flat plateau
3.6V ┤          ▓▓▓▓▓▓▓▓▓▓▓▓
     │                       ▓▓▓▓▓▓▓
3.4V ┤  ← LOW BATTERY (173m)       ▓▓▓
     │                                 ▓▓▓▓
3.0V ┤                                     ▓▓ cliff
2.6V ┤                                        ▓ dead (188m)
     └──────────────────────────────────────────────────
     0    30   60   90   120  150  180 min
```

### Discharge rate by phase

| Phase | Time | Voltage drop | Rate |
|-------|------|-------------|------|
| Surface charge settling | 0–30 min | 4.001 → 3.750 V (−251 mV) | ~8.4 mV/min |
| Main plateau | 30–150 min | 3.750 → 3.470 V (−280 mV) | ~2.3 mV/min |
| Late plateau | 150–173 min | 3.470 → 3.412 V (−58 mV) | ~2.5 mV/min |
| End cliff | 173–188 min | 3.412 → 2.641 V (−771 mV) | ~51 mV/min |

---

## ADC calibration notes

- ADC reads ~4.03 V at what appears to be near-full charge vs LiPo spec max 4.20 V
- Likely ~170 mV systematic under-read at high end (ESP32 ADC non-linearity, no calibration applied)
- At low end the readings show ±10–15 mV noise, some spikes of 20–30 mV
- The current percentage formula `(vbat − 3.0) / 1.2` will show ~84% at "full" rather than 100%
- Suggested calibration: use measured endpoints → `(vbat − 2.65) / (4.00 − 2.65)` maps actual observed range to 0–100%

---

## Implications

- **Runtime with sleep enabled:** significantly longer — sleep draws <1mA vs ~100mA+ awake. Expect 10–20× improvement in deep sleep periods.
- **Warning threshold (3.4V):** fires 15 min before death, appropriate. Could move to 3.5V for ~25 min warning.
- **Usable range:** ~4.0V down to ~3.4V is 173 min (92% of runtime) before the cliff. The cliff is steep and short — no need to worry about the bottom 8%.
- **Battery capacity:** rough estimate — if average current ~80–100mA, 188 min × 90mA ≈ 280 mAh. Consistent with a small 300–400mAh LiPo.
