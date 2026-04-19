# radOS — Nuclear TRNG & Verified Time Source

A Geiger counter firmware for ESP32-C6 that doubles as a **true random number generator** (TRNG) and **nuclear-verified time source**, using radioactive decay as a fundamental entropy and timing reference.

## Theory of Operation

Radioactive decay is a quantum mechanical process — each atom decays independently with a fixed probability per unit time. This produces a Poisson process with two useful properties:

1. **Entropy**: Inter-pulse timing intervals are exponentially distributed and fundamentally unpredictable, making them an ideal entropy source for random number generation.
2. **Timekeeping**: The long-term average count rate (λ) is a physical constant of the source. Given N counts at known λ, elapsed time T = N/λ with uncertainty σ_T = √N/λ.

radOS captures microsecond-resolution pulse timestamps, filters GM tube afterpulses (< 5 ms), applies Von Neumann debiasing and SHA-256 conditioning to produce cryptographic-quality random bytes, and cross-checks the DS3231 RTC against nuclear-derived time estimates using a Kalman filter. Effective entropy rate is ~6-7 bits/sec at ~1650 CPM.

## Hardware BOM

| Component | Description | Notes |
|-----------|-------------|-------|
| ESP32-C6 DevKit | QFN40 rev v0.1, CH343 USB-serial | FQBN: `esp32:esp32:esp32c6` |
| J305 GM Tube | β/γ Geiger-Müller tube, 350-450V operating | 90µs dead time |
| HV Boost Module | NE555-based, drives J305 at ~400V | PWM 14545 Hz, 66.7% duty |
| DS3231 RTC | I²C real-time clock, ±2 ppm TCXO | Battery-backed, aging register |
| WT-40 Electrodes | 10× thoriated tungsten TIG electrodes | ~1600 CPM radioactive source |
| 10kΩ + 10kΩ | Voltage divider on HV feedback | FB_PIN to GND |

## Wiring

```
ESP32-C6 Pin    Signal          Connection
───────────────────────────────────────────
GPIO 4          HV_PIN (PWM)    HV boost converter input
GPIO 0          FB_PIN (ADC)    HV feedback via 10k+10k divider
GPIO 6          TRG_PIN (INT)   GM tube pulse output (RISING edge)
GPIO 1          OUT_PIN         Buzzer/LED click output
GPIO 8          NEO_PIN         Onboard NeoPixel LED
GPIO 12         RTC_SDA         DS3231 I²C data
GPIO 11         RTC_SCL         DS3231 I²C clock
GPIO 13         MODE_PIN        CPS/CPM mode select (LOW=CPS)
```

## Building & Flashing

```bash
# Install arduino-cli and ESP32 board support
arduino-cli core install esp32:esp32

# Compile
arduino-cli compile --fqbn esp32:esp32:esp32c6 .

# Upload (adjust port as needed)
arduino-cli upload --fqbn esp32:esp32:esp32c6 -p /dev/ttyCH343USB0 .

# Monitor serial output
stty -F /dev/ttyCH343USB0 115200 raw -echo && cat /dev/ttyCH343USB0
```

Required libraries (install via `arduino-cli lib install`):
- RTClib 2.1.4
- Adafruit NeoPixel 1.15.4
- Adafruit BusIO 1.17.4

## Serial Commands (115200 baud)

| Command | Description |
|---------|-------------|
| `RAND <n>` | Output n (1-256) random bytes as hex |
| `RANDBIN <n>` | Output n raw binary random bytes |
| `DUMP` | Continuous hex stream (any key to stop) |
| `ENTROPY` | Entropy pool status, rate, health test results |
| `STATUS` | Full system status: CPM, HV, entropy, trust, stratum |
| `TIME` | JSON: RTC time, nuclear time, Kalman fused data, trust |
| `HISTORY` | Last 24 hourly checkpoint records |
| `EVENTS` | Nuclear time verification event log |
| `CALIBRATE` | Force fresh λ calibration (takes 1 hour) |
| `TEST` | Generate 256 random bits, run monobit test |
| `SET <key> <val>` | Configure: `cpm_to_usv`, `log_interval`, `neo_brightness`, `power` |
| `GET [key]` | Read configuration (all keys if no argument) |
| `LOG` | Resume periodic log output |

## Calibration Procedure

1. Place radioactive source next to GM tube in a stable position
2. Power on and wait — radOS logs CPM every second
3. After 1 hour, the first checkpoint is recorded and λ is calibrated
4. After 24 hours, full checkpoint history is available for drift analysis
5. The calibrated λ is stored in NVS and persists across reboots
6. Use `CALIBRATE` to force a fresh calibration if the source changes

## Stratum Hierarchy

| Level | Name | Meaning |
|-------|------|---------|
| UNCAL | Uncalibrated | No data yet |
| CAL | Calibrating | Collecting first hour of data |
| S0 | Stratum-0 | Nuclear source verified, chi-square passing |
| S1 | Stratum-1 | RTC disciplined by nuclear (aging register adjusted) |
| HOLD | Holdover | Source removed, running on RTC with growing uncertainty |

## Trust Score (0-100%)

Weighted composite of:
- **30 pts** — Source present and counting (no starvation/anomaly)
- **20 pts** — λ calibrated from data
- **20 pts** — Chi-square goodness-of-fit test passing
- **20 pts** — RTC-nuclear drift agreement (< 2 ppm)
- **10 pts** — Uptime (more checkpoints = more confidence)

## TRNG Architecture

```
GM Tube Pulses
    │
    ▼ (ISR, µs timestamps)
Ring Buffer (256 entries)
    │
    ▼ (main loop)
Inter-pulse Intervals (ΔT)
    │
    ▼
Afterpulse Filter (discard ΔT < 5 ms)
    │
    ▼
Von Neumann Debiasing (pairs: ΔT₁<ΔT₂→1, ΔT₁>ΔT₂→0)
    │
    ├──▶ NIST SP 800-90B Health Tests
    │      • Repetition Count Test (C=41)
    │      • Adaptive Proportion Test (W=1024, C=656)
    │
    ▼
Entropy Pool (256-bit, XOR folding)
    │
    ▼
SHA-256 Conditioning (mbedTLS)
    │
    ├──▶ Continuous Monobit Self-test (every 1000 bytes)
    │
    ▼
Random Bytes Output (serial)
```

## NeoPixel Status

| Color | Meaning |
|-------|---------|
| Breathing blue | Calibrating (collecting data) |
| Steady green | Stratum-0/1 verified (brightness scales with trust) |
| Yellow | Holdover mode (source removed) |
| Red | Alarm (source anomaly, starvation, or health test failure) |
| White/yellow flash | Pulse detected |

## Linux hwrng Bridge

Feed nuclear random bytes into the kernel entropy pool:

```bash
sudo python3 bin/rados_hwrng.py /dev/ttyCH343USB0
```

## Graceful Degradation

- **RTC fails**: Falls back to `millis()` with relative timestamps (`T+HH:MM:SS`)
- **Source weak**: Entropy output rate reduced automatically (pool must refill)
- **Source removed**: Holdover mode, RTC continues, uncertainty grows
- **Health test fail**: TRNG output blocked until source recovers

## License

MIT
