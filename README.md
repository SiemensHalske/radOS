# radOS — Nuclear TRNG & Verified Time Source

A Geiger counter firmware for ESP32-C6 that doubles as a **true random number generator** (TRNG) and **nuclear-verified time source**, using radioactive decay as a fundamental entropy and timing reference.

## Theory of Operation

### Background: Radioactive Decay

Radioactive decay is the spontaneous disintegration of an unstable atomic nucleus into a more stable configuration, releasing energy in the form of particles or electromagnetic radiation. It is governed entirely by quantum mechanics — there is no classical mechanism by which one can predict *when* a specific nucleus will decay. This is not a limitation of measurement; it is a fundamental feature of quantum probability.

#### Decay Modes

The three primary decay modes relevant to radiation detection are:

| Mode | Emission | Penetration | Relevance to J305 |
|------|----------|-------------|-------------------|
| **Alpha (α)** | $^4_2\text{He}$ nucleus | ~3–7 cm in air; stopped by paper | Not detected — stopped before tube window |
| **Beta (β)** | Electron (β⁻) or positron (β⁺) | mm–cm in solids | Detected through thin mica/glass end window |
| **Gamma (γ)** | High-energy photon | Metres in air; cm–cm in dense shielding | Detected via secondary electron ionisation in tube gas |

The J305 tube used in radOS is primarily a **β/γ detector**. Thoriated tungsten electrodes (WT-40) are the radioactive source — they undergo β⁻ decay from Thorium-232 and its daughters, which also produce γ photons.

#### The Decay Law

A macroscopic sample of $N_0$ atoms of a radioactive isotope with decay constant $\lambda$ (s⁻¹) evolves according to:

$$N(t) = N_0 \, e^{-\lambda t}$$

The **half-life** $T_{1/2}$ — the time for half the atoms to decay — is:

$$T_{1/2} = \frac{\ln 2}{\lambda}$$

Thorium-232 has $T_{1/2} \approx 1.4 \times 10^{10}$ years, making its activity essentially constant on any human timescale. For radOS this is essential: λ is stable, so count-derived time estimates do not drift due to source depletion.

#### Activity and Count Rate

The **activity** $A$ of a source is the expected number of decays per second:

$$A = \lambda N(t)$$

measured in Becquerels (Bq; 1 Bq = 1 decay/s). What radOS observes is not $A$ directly but the **detected count rate** $\dot{n}$, which is smaller by the geometric and intrinsic efficiency $\varepsilon$ of the GM tube:

$$\dot{n} = \varepsilon \cdot A$$

For the WT-40 electrode bundle at the tube geometry used, $\dot{n} \approx 1650$ CPM = 27.5 counts/sec. The absolute value of $A$ is not needed — radOS calibrates $\lambda_{\text{eff}} = \dot{n}$ directly from accumulated counts, absorbing $\varepsilon$ into the calibration.

#### GM Tube Detection Mechanism

A Geiger-Müller tube is a gas-filled cylindrical capacitor held near its breakdown voltage (~400 V for the J305). When an ionising particle enters:

1. It ionises gas atoms along its track, creating ion–electron pairs.
2. Electrons accelerate toward the anode wire, gaining enough energy to cause **avalanche ionisation** — a Townsend cascade.
3. The cascade produces a macroscopic current pulse (~µs rise time).
4. UV photons from the discharge can trigger secondary avalanches elsewhere in the tube — **afterpulses**. A halogen-quenched fill gas (or external quench resistor) terminates the discharge within ~90 µs (the J305 dead time).
5. The remaining positive ion sheath slowly drifts to the cathode (~100–500 µs), after which the tube is ready for the next event.

radOS enforces a software dead-time of **5 ms** (much longer than the 90 µs tube dead time) to suppress any residual afterpulse artefacts that could introduce inter-pulse correlations into the entropy pool.

---

### Radioactive Decay as a Physical Process

Radioactive decay is a quantum mechanical process — each nucleus in a sample decays independently with a constant probability per unit time `λ` (the decay constant). No memory, no correlation, no external influence. The number of decays in a fixed interval T follows a **Poisson distribution**:

$$P(k) = \frac{(\lambda T)^k e^{-\lambda T}}{k!}$$

where `k` is the observed count. Crucially, the **inter-arrival times** between successive decay events follow an **exponential distribution**:

$$f(\Delta t) = \lambda \, e^{-\lambda \Delta t}$$

This exponential distribution is memoryless — the probability of the next decay is independent of when the last one occurred. It is this irreducible quantum randomness that radOS exploits.

### Entropy Source

Each inter-pulse interval $\Delta t_i$ carries approximately $\log_2(\lambda \Delta t_i) + \log_2(e)$ bits of Shannon entropy. For a raw exponential distribution with mean $1/\lambda$, the differential entropy is:

$$H = 1 - \log_2(\lambda) \text{ bits per sample (continuous)}$$

At ~1650 CPM (λ ≈ 27.5 pulses/sec), after afterpulse filtering and **Von Neumann debiasing** (which discards ~50% of pairs), and accounting for the 1 bit per debiased pair output, the net entropy throughput is approximately **6–7 bits/sec**.

**Von Neumann debiasing** removes bias from the raw timing bits without requiring knowledge of the underlying distribution. Consecutive interval pairs $(\Delta t_1, \Delta t_2)$ are compared:
- $\Delta t_1 < \Delta t_2$ → emit `1`
- $\Delta t_1 > \Delta t_2$ → emit `0`
- $\Delta t_1 = \Delta t_2$ → discard (extremely rare at µs resolution)

This produces unbiased bits regardless of the shape of the underlying distribution, requiring only that consecutive samples are i.i.d. (satisfied by memoryless decay).

The debiased bits are XOR-folded into a **256-bit entropy pool** and conditioned through **SHA-256** (mbedTLS), which compresses the pool into a uniform output block. SHA-256 conditioning is a NIST SP 800-90B–approved construction that prevents an adversary from inferring pool state even with partial knowledge of the inputs.

### Nuclear Timekeeping

The mean count rate λ is a **physical constant** of the source — it depends only on the isotope's half-life, source geometry, and tube efficiency, none of which change on human timescales. This makes accumulated counts a reliable clock:

$$T_{\text{nuclear}} = \frac{N}{\hat{\lambda}}$$

where $N$ is the total count and $\hat{\lambda}$ is the calibrated rate (counts/sec). The **1σ timing uncertainty** follows from Poisson statistics:

$$\sigma_T = \frac{\sqrt{N}}{\hat{\lambda}} = \frac{1}{\sqrt{\hat{\lambda} \cdot T}}$$

At 1650 CPM (27.5 Hz), after 1 hour (N = 99,000 counts):

$$\sigma_T = \frac{\sqrt{99000}}{27.5} \approx 11.4 \text{ sec}$$

After 24 hours (N ≈ 2.376M counts):

$$\sigma_T \approx \frac{\sqrt{2376000}}{27.5} \approx 56 \text{ sec}$$

This is coarse compared to a crystal oscillator, but it is **physically grounded** — its accuracy depends on quantum mechanics, not oscillator aging or network trust.

### Kalman Filter: Fusing Nuclear Time with the RTC

The DS3231 RTC has a ±2 ppm TCXO, corresponding to ~63 ms/day drift at worst case. radOS fuses the RTC reading with the nuclear time estimate using a **1D Kalman filter**:

**State**: RTC offset error $x_k$ (seconds the RTC is ahead of true time)

**Predict step** (between checkpoints, over interval $\Delta T$):
$$\hat{x}_{k|k-1} = \hat{x}_{k-1|k-1}$$
$$P_{k|k-1} = P_{k-1|k-1} + Q \cdot \Delta T$$

where $Q$ is the process noise (RTC drift variance per second, derived from the ±2 ppm spec).

**Update step** (at each hourly checkpoint, when $z_k = T_{\text{RTC}} - T_{\text{nuclear}}$ is observed):
$$K_k = \frac{P_{k|k-1}}{P_{k|k-1} + R_k}$$
$$\hat{x}_{k|k} = \hat{x}_{k|k-1} + K_k (z_k - \hat{x}_{k|k-1})$$
$$P_{k|k} = (1 - K_k) P_{k|k-1}$$

where $R_k = \sigma_T^2$ is the nuclear time measurement variance at checkpoint $k$.

Over successive checkpoints, the filter drives RTC correction toward the nuclear estimate, and the uncertainty $P$ shrinks as more counts accumulate. When the source is removed (**holdover mode**), $P$ grows at the rate $Q$ per second — the filter honestly reports its growing ignorance.

The DS3231 aging register is adjusted when the long-term drift estimate stabilises (Stratum-1), trimming the RTC's frequency to match the nuclear reference.

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
