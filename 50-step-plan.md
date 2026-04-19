# radOS — Nuclear TRNG + Verified Time Source
## 50-Step Implementation Plan

### Phase 1: Entropy Capture (Steps 1–10) ✅ COMPLETE
Capture raw timing data from radioactive decay events for use as entropy.

1. **Add microsecond timestamp ring buffer in ISR** — Record `micros()` at each pulse in a circular buffer (e.g., 256 entries). This is the raw entropy source.
2. **Compute inter-pulse intervals** — In the main loop, subtract consecutive timestamps to get ΔT (time between decays). These intervals are exponentially distributed = high entropy.
3. **Implement Von Neumann debiasing** — Take pairs of intervals: if ΔT₁ < ΔT₂ → output 1, if ΔT₁ > ΔT₂ → output 0, if equal → discard. Removes any bias from the source.
4. **Create entropy pool (256-bit)** — Accumulate debiased bits into a 32-byte pool using XOR folding.
5. **Add entropy counter** — Track how many raw bits have been mixed into the pool since last drain. Don't serve random bytes until enough entropy is collected.
6. **Implement SHA-256 conditioning** — Hash the entropy pool to produce uniform output bytes. Use mbedTLS (built into ESP32 SDK).
7. **Create `trng.h` / `trng.cpp` module** — Expose: `trng_init()`, `trng_available()` (bits of entropy ready), `trng_read(buf, len)`.
8. **Add serial command `RAND <n>`** — Request n random bytes, output as hex over serial.
9. **Add entropy rate monitoring** — Log bits/second of raw entropy to serial for diagnostics. At 1600 CPM this should be ~13 raw bits/sec, ~6.5 debiased bits/sec.
10. **Unit test debiasing** — Print raw intervals and debiased bits to serial, verify distribution looks uniform (rough chi-square by eye).

### Phase 2: TRNG Output Interfaces (Steps 11–18) ✅ COMPLETE
Make the random bytes accessible over serial and local hardware.

11. **Serial command `ENTROPY`** — Return entropy pool status: bits available, rate, total bytes served.
12. **Serial command `STATUS`** — Return full system status: CPM, HV, RTC time, entropy rate, trust score, uptime.
13. **Serial binary mode** — Command `RANDBIN <n>` outputs raw bytes (not hex) for piping directly into `/dev/random` or files.
14. **Entropy health indicator on NeoPixel** — Pulse green when pool is full, yellow when filling, red when drained.
15. **Rate limiting** — Cap output to available entropy. Return error if pool is drained faster than decay events refill it.
16. **Linux `/dev/hwrng` bridge script** — Python/shell script on host that reads serial TRNG output and feeds it into the kernel entropy pool.
17. **Serial command `DUMP`** — Continuous hex stream mode for bulk entropy collection. Send any key to stop.
18. **Auto-detect serial commands vs log mode** — If host sends a command, switch from log output to command mode. `LOG` to resume logging.

### Phase 3: Nuclear Time Source — Calibration (Steps 19–28) ✅ COMPLETE
Use known statistical properties of radioactive decay to build a trustworthy time reference.

19. **Track expected vs actual count rate** — Maintain a long-term running average of CPM. Poisson process: if rate is λ, then N counts in time T has mean λT and std √(λT).
20. **Implement RTC drift estimator** — Compare DS3231 elapsed time vs expected time derived from count statistics. Over hours, systematic drift becomes detectable.
21. **Add count accumulator with RTC checkpoints** — Every hour, record {RTC timestamp, total counts}. Store in a circular buffer (24 entries = 1 day).
22. **Compute λ (decay rate) from settled data** — After first 24h of data, calculate average counts/second. This is the calibrated source activity.
23. **Store λ calibration in EEPROM/NVS** — Persist the calibrated decay rate across reboots using ESP32 NVS (non-volatile storage).
24. **Implement chi-square goodness-of-fit** — Each hour, test if the observed count distribution matches Poisson with the calibrated λ. Flag anomalies.
25. **Add temperature reading from DS3231** — The DS3231 has a built-in temperature sensor. Log it alongside counts to correlate any environmental effects.
26. **Detect source removal/change** — If CPM drops below 3σ of expected rate for >5 minutes, flag "source anomaly" — someone removed the electrodes or the tube failed.
27. **Implement Poisson confidence interval for time** — Given N counts at known λ, elapsed time T = N/λ with uncertainty σ_T = √N/λ. This is the nuclear-derived time estimate.
28. **Display time uncertainty on serial** — Show: `RTC: 17:52:03 | Nuclear: 17:52:01 ±2.3s | Δ=2s`

### Phase 4: Time Verification & Discipline (Steps 29–38) ✅ COMPLETE
Cross-check RTC against nuclear time and correct drift.

29. **Implement RTC discipline loop** — If nuclear time consistently leads/lags RTC by >1s over an hour, apply a correction factor to the RTC aging register.
30. **DS3231 aging register control** — The DS3231 has an aging offset register that adjusts crystal frequency by ~0.1 ppm/step. Use it for fine correction.
31. **Add Kalman filter for time fusion** — Fuse RTC (precise short-term) with nuclear count (trustworthy long-term) using a simple 1D Kalman filter. Output: best time estimate + uncertainty.
32. **Implement holdover mode** — If source is removed, continue on RTC alone but flag "unverified" and grow the uncertainty estimate over time.
33. **Implement "trust score"** — 0-100% based on: source present, chi-square pass, RTC-nuclear agreement, pool entropy level, uptime.
34. **Stratum-like hierarchy** — Define trust levels: Stratum-0 = nuclear source verified, Stratum-1 = RTC disciplined by nuclear, Stratum-2 = RTC holdover (degrading).
35. **Log time verification events** — Store pass/fail of each hourly check in NVS. Retrievable over serial.
36. **Watchdog for count starvation** — If no pulses arrive for >10 seconds (statistically impossible at 1600 CPM), trigger HV check and alarm.
37. **Add serial command `TIME`** — Return JSON: `{"rtc": "...", "nuclear": "...", "delta_ms": ..., "uncertainty_ms": ..., "trust": 87}`
38. **Add serial command `CALIBRATE`** — Force a fresh λ calibration over the next hour, store result in NVS.

### Phase 5: Polish & Usability (Steps 39–46) ✅ COMPLETE
Local usability and configuration.

39. **NeoPixel trust indicator** — Steady green = Stratum-0, breathing blue = calibrating, yellow = holdover, red = anomaly.
40. **Serial command `SET <key> <value>`** — Configure parameters: `SET tube_type J305`, `SET source_cpm 1600`, etc. Store in NVS.
41. **Serial command `GET <key>`** — Read back any configuration parameter.
42. **Serial command `CALIBRATE`** — Force a fresh λ calibration over the next hour, store result in NVS.
43. **Serial command `HISTORY`** — Print last 24 hourly checkpoint records: timestamp, counts, λ, drift, chi-square result.
44. **Power management** — Implement light sleep between measurements if running on battery. Wake on pulse interrupt.
45. **Boot self-test** — On startup, verify: HV OK, RTC responding, entropy source producing, NVS readable. Report pass/fail for each.
46. **Graceful degradation** — If RTC fails, run on millis() only. If source is weak, reduce entropy output rate. Always keep counting.

### Phase 6: Hardening & Documentation (Steps 47–50) ✅ COMPLETE
Make it robust and reproducible.

47. **NIST SP 800-90B health tests** — Implement repetition count test and adaptive proportion test on the raw entropy source. Required for any serious RNG.
48. **Continuous self-test** — Every 1000 bytes output, run a basic frequency test. If it fails, stop serving and flag alarm.
49. **Write README** — Document: theory of operation, hardware BOM, wiring diagram (text-based), calibration procedure, serial command reference.
50. **Tag v1.0 release** — Clean up code, tag as v1.0. You now have a nuclear-verified time source and true random number generator in a box.
