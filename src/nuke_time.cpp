#include "nuke_time.h"
#include "rtc.h"
#include <Preferences.h>
#include <math.h>

// --- State ---
static Preferences nvs;

// Checkpoint ring buffer
static nt_checkpoint_t checkpoints[NT_CHECKPOINT_COUNT];
static uint8_t cp_count = 0; // Number of valid checkpoints
static uint8_t cp_write = 0; // Next write index

// Calibration
static float lambda_cps = 0.0f; // Calibrated counts per second
static bool calibrated = false;
static bool calibrating = false;

// Running state
static uint32_t last_cp_epoch = 0;     // RTC epoch at last checkpoint
static uint32_t last_cp_counts = 0;    // Total counts at last checkpoint
static uint32_t update_epoch = 0;      // Latest RTC epoch seen
static uint32_t update_counts = 0;     // Latest total counts seen
static uint32_t prev_counts = 0;       // Counts at previous update (for starvation)
static uint32_t last_count_change = 0; // millis() when counts last changed

// Anomaly detection (step 26)
static uint32_t anomaly_start = 0; // millis() when anomaly first detected
static bool source_anomaly_flag = false;

// Drift tracking (step 20)
static float drift_ppm = 0.0f;

// Step 31: Kalman filter state
static float kf_offset = 0.0f;                            // Estimated RTC offset (seconds)
static float kf_drift = 0.0f;                             // Estimated drift rate (sec/sec)
static float kf_P[2][2] = {{100.0f, 0.0f}, {0.0f, 1.0f}}; // Covariance
static bool kf_initialized = false;

// Step 32: Holdover
static uint32_t holdover_start = 0;  // millis() when holdover began
static float holdover_uncert = 0.0f; // Growing uncertainty in holdover

// Step 33: Trust score
static uint8_t trust_score = 0;

// Step 35: Event log in NVS
static uint8_t event_count = 0;
static uint8_t event_write = 0;

// Step 36: Starvation
static bool starved = false;

// Step 29: Discipline state
static bool discipline_applied = false;

// ============================================================
// Step 23: NVS persistence
// ============================================================

bool nt_save_calibration()
{
    nvs.begin("nuketime", false);
    nvs.putFloat("lambda", lambda_cps);
    nvs.putBool("cal", calibrated);
    nvs.end();
    return true;
}

bool nt_load_calibration()
{
    nvs.begin("nuketime", true);
    lambda_cps = nvs.getFloat("lambda", 0.0f);
    calibrated = nvs.getBool("cal", false);
    nvs.end();
    return calibrated && lambda_cps > 0.0f;
}

// ============================================================
// Init
// ============================================================

void nt_init()
{
    cp_count = 0;
    cp_write = 0;
    last_cp_epoch = 0;
    last_cp_counts = 0;
    anomaly_start = 0;
    source_anomaly_flag = false;
    drift_ppm = 0.0f;
    calibrating = false;
    prev_counts = 0;
    last_count_change = millis();
    starved = false;
    holdover_start = 0;
    holdover_uncert = 0.0f;
    trust_score = 0;
    discipline_applied = false;
    kf_initialized = false;
    kf_offset = 0.0f;
    kf_drift = 0.0f;
    kf_P[0][0] = 100.0f;
    kf_P[0][1] = 0.0f;
    kf_P[1][0] = 0.0f;
    kf_P[1][1] = 1.0f;

    // Load event log count from NVS
    nvs.begin("nuketime", true);
    event_count = nvs.getUChar("evcnt", 0);
    event_write = nvs.getUChar("evwr", 0);
    nvs.end();

    // Try to load stored calibration
    if (nt_load_calibration())
    {
        // Have a stored lambda — use it
    }
    else
    {
        calibrated = false;
        lambda_cps = 0.0f;
        calibrating = true;
    }
}

// ============================================================
// Step 24: Chi-square goodness-of-fit for Poisson
// ============================================================
// Split the checkpoint interval into sub-bins and test if count
// distribution matches Poisson(lambda * bin_duration).
// We use 60 x 1-minute bins within each 1-hour checkpoint.
// Simplified: compute chi2 = sum((Oi - Ei)^2 / Ei) for the
// single interval as a variance check.
//
// For a Poisson process with expected count E in time T:
//   chi2 ≈ (O - E)^2 / E
// With 1 DOF, critical value at p=0.05 is 3.84, at p=0.01 is 6.63

static float compute_chi2(uint32_t observed, float expected)
{
    if (expected < 1.0f)
        return 0.0f;
    float diff = (float)observed - expected;
    return (diff * diff) / expected;
}

// ============================================================
// Step 35: Event log — store verification events in NVS
// ============================================================
// Each event is a short string stored as "ev0".."ev23" in NVS.

static void log_event(const char *msg)
{
    char key[8];
    snprintf(key, sizeof(key), "ev%d", event_write);
    nvs.begin("nuketime", false);
    nvs.putString(key, msg);
    event_write = (event_write + 1) % NT_EVENT_LOG_SIZE;
    if (event_count < NT_EVENT_LOG_SIZE)
        event_count++;
    nvs.putUChar("evcnt", event_count);
    nvs.putUChar("evwr", event_write);
    nvs.end();
}

uint8_t nt_event_count()
{
    return event_count;
}

bool nt_get_event(uint8_t idx, char *buf, size_t len)
{
    if (idx >= event_count)
        return false;
    uint8_t actual = (event_write - event_count + idx + NT_EVENT_LOG_SIZE) % NT_EVENT_LOG_SIZE;
    char key[8];
    snprintf(key, sizeof(key), "ev%d", actual);
    nvs.begin("nuketime", true);
    String s = nvs.getString(key, "");
    nvs.end();
    if (s.length() == 0)
        return false;
    strncpy(buf, s.c_str(), len - 1);
    buf[len - 1] = '\0';
    return true;
}

// ============================================================
// Step 31: 1D Kalman filter for RTC offset + drift
// ============================================================
// State: [offset, drift_rate]  (offset = nuclear_time - rtc_time)
// Prediction: offset += drift_rate * dt
// Measurement: nuclear-derived offset with uncertainty from √N/λ

static void kalman_predict(float dt)
{
    // State prediction
    kf_offset += kf_drift * dt;

    // Covariance prediction: P = F*P*F' + Q
    // F = [[1, dt], [0, 1]]
    // Q = process noise (RTC wander + source variance)
    float q_offset = 1e-6f * dt; // ~1 µs²/s RTC wander
    float q_drift = 1e-12f * dt; // Very slow drift change

    float p00 = kf_P[0][0] + dt * (kf_P[1][0] + kf_P[0][1]) + dt * dt * kf_P[1][1] + q_offset;
    float p01 = kf_P[0][1] + dt * kf_P[1][1];
    float p10 = kf_P[1][0] + dt * kf_P[1][1];
    float p11 = kf_P[1][1] + q_drift;

    kf_P[0][0] = p00;
    kf_P[0][1] = p01;
    kf_P[1][0] = p10;
    kf_P[1][1] = p11;
}

static void kalman_update(float measured_offset, float measurement_var)
{
    // Innovation
    float y = measured_offset - kf_offset;

    // Innovation covariance: S = H*P*H' + R   (H = [1, 0])
    float S = kf_P[0][0] + measurement_var;
    if (S < 1e-10f)
        return;

    // Kalman gain: K = P*H' / S
    float K0 = kf_P[0][0] / S;
    float K1 = kf_P[1][0] / S;

    // State update
    kf_offset += K0 * y;
    kf_drift += K1 * y;

    // Covariance update: P = (I - K*H) * P
    float p00 = (1.0f - K0) * kf_P[0][0];
    float p01 = (1.0f - K0) * kf_P[0][1];
    float p10 = kf_P[1][0] - K1 * kf_P[0][0];
    float p11 = kf_P[1][1] - K1 * kf_P[0][1];

    kf_P[0][0] = p00;
    kf_P[0][1] = p01;
    kf_P[1][0] = p10;
    kf_P[1][1] = p11;
}

// ============================================================
// Step 29: RTC discipline — adjust aging register based on drift
// ============================================================
// drift_ppm > 0 means RTC is slow (nuclear sees more time than RTC)
// DS3231 aging: positive = slow crystal. So if RTC is fast, increase aging.
// Each aging step ≈ 0.1 ppm.

static void discipline_rtc(float drift_ppm_val)
{
    // Only discipline if drift is significant (>1 ppm) and stable
    if (fabsf(drift_ppm_val) < 1.0f)
        return;

    // Compute correction: negative drift_ppm means RTC is fast
    // To slow down RTC (make it less fast), increase aging offset
    // drift_ppm > 0 → RTC slow → decrease aging (speed up crystal)
    // drift_ppm < 0 → RTC fast → increase aging (slow down crystal)
    int8_t steps = (int8_t)(-drift_ppm_val / 0.1f);

    // Clamp to ±5 steps per adjustment to be conservative
    if (steps > 5)
        steps = 5;
    if (steps < -5)
        steps = -5;
    if (steps == 0)
        return;

    rtc_adjust_aging(steps);
    discipline_applied = true;

    // Log the discipline event
    char msg[48];
    snprintf(msg, sizeof(msg), "DISC drift=%.1fppm aging%+d", drift_ppm_val, steps);
    log_event(msg);
}

// ============================================================
// Step 33: Trust score computation (0-100)
// ============================================================

static void compute_trust()
{
    uint8_t score = 0;

    // Source present and counting (30 points)
    if (!starved && !source_anomaly_flag)
        score += 30;
    else if (!starved)
        score += 15;

    // Calibrated (20 points)
    if (calibrated)
        score += 20;
    else if (calibrating && cp_count > 0)
        score += 10;

    // Chi-square passing (20 points) — check last checkpoint
    if (cp_count > 0)
    {
        const nt_checkpoint_t *last = nt_get_checkpoint(cp_count - 1);
        if (last && last->chi2_pass)
            score += 20;
        else
            score += 5; // At least we have data
    }

    // RTC-nuclear agreement (20 points) — based on drift magnitude
    if (calibrated)
    {
        float abs_drift = fabsf(drift_ppm);
        if (abs_drift < 2.0f)
            score += 20;
        else if (abs_drift < 10.0f)
            score += 15;
        else if (abs_drift < 50.0f)
            score += 10;
        else
            score += 5;
    }

    // Uptime bonus (10 points) — more checkpoints = more confidence
    if (cp_count >= 24)
        score += 10;
    else if (cp_count >= 6)
        score += 7;
    else if (cp_count >= 1)
        score += 3;

    trust_score = (score > 100) ? 100 : score;
}

// ============================================================
// Step 22: Compute lambda from checkpoint data
// ============================================================

static void recalculate_lambda()
{
    if (cp_count < NT_MIN_CALIBRATION_H)
        return;

    // Use all available checkpoints to compute weighted average lambda
    float total_counts = 0;
    float total_seconds = 0;

    for (uint8_t i = 0; i < cp_count; i++)
    {
        uint8_t idx = (cp_write - cp_count + i + NT_CHECKPOINT_COUNT) % NT_CHECKPOINT_COUNT;
        if (checkpoints[idx].lambda > 0.0f)
        {
            // Reconstruct interval duration and counts
            total_counts += checkpoints[idx].lambda * NT_CHECKPOINT_SEC;
            total_seconds += NT_CHECKPOINT_SEC;
        }
    }

    if (total_seconds > 0)
    {
        lambda_cps = total_counts / total_seconds;
        calibrated = true;
        calibrating = false;
    }
}

// ============================================================
// Main update — called once per second from loop()
// ============================================================

void nt_update(uint32_t rtc_epoch, uint32_t total_counts, float temp_c)
{
    update_epoch = rtc_epoch;
    update_counts = total_counts;

    // ---- Step 36: Starvation detection ----
    if (total_counts != prev_counts)
    {
        prev_counts = total_counts;
        last_count_change = millis();
        starved = false;
    }
    else if ((millis() - last_count_change) > (NT_STARVATION_SEC * 1000UL))
    {
        if (!starved)
        {
            starved = true;
            log_event("STARVE no pulses >10s");
        }
    }

    // First call — seed the checkpoint baseline
    if (last_cp_epoch == 0)
    {
        last_cp_epoch = rtc_epoch;
        last_cp_counts = total_counts;
        prev_counts = total_counts;
        last_count_change = millis();
        return;
    }

    uint32_t elapsed = rtc_epoch - last_cp_epoch;

    // ---- Step 26: Source anomaly detection ----
    if (calibrated && elapsed >= 10)
    {
        float expected = lambda_cps * (float)elapsed;
        uint32_t observed = total_counts - last_cp_counts;
        float sigma = sqrtf(expected);

        if (sigma > 0 && ((float)observed < expected - NT_ANOMALY_SIGMA * sigma))
        {
            if (anomaly_start == 0)
            {
                anomaly_start = millis();
            }
            else if ((millis() - anomaly_start) > (NT_ANOMALY_SEC * 1000UL))
            {
                if (!source_anomaly_flag)
                {
                    source_anomaly_flag = true;
                    holdover_start = millis();
                    holdover_uncert = 0.0f;
                    log_event("ANOM source anomaly");
                }
            }
        }
        else
        {
            if (source_anomaly_flag)
            {
                log_event("ANOM resolved");
            }
            anomaly_start = 0;
            source_anomaly_flag = false;
            holdover_start = 0;
        }
    }

    // ---- Step 32: Holdover — grow uncertainty when source is gone ----
    if (source_anomaly_flag && holdover_start > 0)
    {
        float holdover_sec = (float)(millis() - holdover_start) / 1000.0f;
        // DS3231 spec: ±2 ppm = 2e-6 sec/sec drift
        holdover_uncert = holdover_sec * 2e-6f * holdover_sec; // quadratic growth
    }

    // ---- Step 31: Kalman filter update (each second) ----
    if (calibrated && !source_anomaly_flag && elapsed >= 1)
    {
        uint32_t counts_since_cp = total_counts - last_cp_counts;
        if (counts_since_cp > 10)
        {
            float nuclear_elapsed = (float)counts_since_cp / lambda_cps;
            float rtc_elapsed = (float)elapsed;
            float measured_offset = nuclear_elapsed - rtc_elapsed;
            // Measurement variance: (√N / λ)²  = N / λ²
            float meas_var = (float)counts_since_cp / (lambda_cps * lambda_cps);

            if (!kf_initialized)
            {
                kf_offset = measured_offset;
                kf_drift = 0.0f;
                kf_P[0][0] = meas_var;
                kf_P[0][1] = 0.0f;
                kf_P[1][0] = 0.0f;
                kf_P[1][1] = 1e-6f;
                kf_initialized = true;
            }
            else
            {
                kalman_predict(1.0f); // 1 second step
                kalman_update(measured_offset, meas_var);
            }
        }
    }

    // ---- Step 21: Hourly checkpoint ----
    if (elapsed >= NT_CHECKPOINT_SEC)
    {
        uint32_t interval_counts = total_counts - last_cp_counts;
        float interval_lambda = (float)interval_counts / (float)elapsed;

        // Step 24: Chi-square test
        float expected = calibrated ? (lambda_cps * (float)elapsed) : (float)interval_counts;
        float chi2 = calibrated ? compute_chi2(interval_counts, expected) : 0.0f;
        bool chi2_pass = (chi2 < 6.63f);

        // Store checkpoint
        checkpoints[cp_write] = {
            .epoch = rtc_epoch,
            .counts = total_counts,
            .temp_c = temp_c,
            .lambda = interval_lambda,
            .chi2 = chi2,
            .chi2_pass = chi2_pass};
        cp_write = (cp_write + 1) % NT_CHECKPOINT_COUNT;
        if (cp_count < NT_CHECKPOINT_COUNT)
            cp_count++;

        // Step 20: Drift estimation
        if (calibrated && interval_counts > 0)
        {
            float nuclear_elapsed = (float)interval_counts / lambda_cps;
            float rtc_elapsed = (float)elapsed;
            float drift_sec = nuclear_elapsed - rtc_elapsed;
            drift_ppm = (drift_sec / rtc_elapsed) * 1e6f;
        }

        // Step 22: Recalculate lambda
        recalculate_lambda();

        // Step 29: RTC discipline — adjust aging if drift detected
        if (calibrated && cp_count >= 2 && !source_anomaly_flag)
        {
            discipline_rtc(drift_ppm);
        }

        // Step 35: Log checkpoint event
        char msg[48];
        snprintf(msg, sizeof(msg), "CP lam=%.2f chi2=%.1f %s",
                 interval_lambda, chi2, chi2_pass ? "PASS" : "FAIL");
        log_event(msg);

        // Update baseline
        last_cp_epoch = rtc_epoch;
        last_cp_counts = total_counts;

        // Step 23: Auto-save if calibrated
        if (calibrated)
        {
            nt_save_calibration();
        }
    }

    // ---- Step 33: Compute trust score every update ----
    compute_trust();
}

// ============================================================
// Step 27: Nuclear time estimate with uncertainty
// ============================================================

nt_time_t nt_get_time()
{
    nt_time_t t = {0, 0, 0, false};

    if (!calibrated || lambda_cps <= 0.0f || last_cp_epoch == 0)
        return t;

    uint32_t counts_since_cp = update_counts - last_cp_counts;
    float nuclear_elapsed = (float)counts_since_cp / lambda_cps;

    // σ_T = √N / λ  (uncertainty from Poisson counting statistics)
    float uncertainty = sqrtf((float)counts_since_cp) / lambda_cps;

    // RTC elapsed since last checkpoint
    float rtc_elapsed = (float)(update_epoch - last_cp_epoch);

    t.elapsed_sec = nuclear_elapsed;
    t.uncertainty_sec = uncertainty;
    t.delta_ms = (int32_t)((nuclear_elapsed - rtc_elapsed) * 1000.0f);
    t.valid = (counts_since_cp > 100); // Need some counts for meaningful estimate

    return t;
}

// ============================================================
// Accessors
// ============================================================

float nt_get_lambda()
{
    return lambda_cps;
}

bool nt_is_calibrated()
{
    return calibrated;
}

nt_stratum_t nt_get_stratum()
{
    if (!calibrated && !calibrating)
        return NT_STRATUM_UNCAL;
    if (calibrating)
        return NT_STRATUM_CAL;
    if (source_anomaly_flag)
        return NT_STRATUM_HOLDOVER;
    if (discipline_applied)
        return NT_STRATUM_1;
    return NT_STRATUM_0;
}

bool nt_source_anomaly()
{
    return source_anomaly_flag;
}

float nt_get_drift_ppm()
{
    return drift_ppm;
}

uint8_t nt_get_trust()
{
    return trust_score;
}

bool nt_is_starved()
{
    return starved;
}

nt_fused_t nt_get_fused()
{
    nt_fused_t f = {0, 0, 0};
    if (!kf_initialized)
        return f;
    f.offset_sec = kf_offset;
    f.uncertainty_sec = sqrtf(kf_P[0][0]);
    // In holdover, grow uncertainty
    if (source_anomaly_flag && holdover_start > 0)
    {
        f.uncertainty_sec += holdover_uncert;
    }
    f.drift_rate = kf_drift;
    return f;
}

uint8_t nt_checkpoint_count()
{
    return cp_count;
}

const nt_checkpoint_t *nt_get_checkpoint(uint8_t idx)
{
    if (idx >= cp_count)
        return nullptr;
    uint8_t actual = (cp_write - cp_count + idx + NT_CHECKPOINT_COUNT) % NT_CHECKPOINT_COUNT;
    return &checkpoints[actual];
}

void nt_recalibrate()
{
    calibrated = false;
    calibrating = true;
    lambda_cps = 0.0f;
    cp_count = 0;
    cp_write = 0;
    drift_ppm = 0.0f;
    kf_initialized = false;
    discipline_applied = false;
    holdover_start = 0;
    holdover_uncert = 0.0f;
    last_cp_epoch = 0;
    log_event("RECAL started");
}
