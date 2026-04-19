#pragma once

#include "config.h"

// --- Configuration ---
#define NT_CHECKPOINT_COUNT  24    // Hourly checkpoints (24h ring buffer)
#define NT_CHECKPOINT_SEC    3600  // Seconds per checkpoint (1 hour)
#define NT_MIN_CALIBRATION_H 1    // Minimum hours before calibration is valid
#define NT_ANOMALY_SIGMA     3.0f // Sigma threshold for source anomaly
#define NT_ANOMALY_SEC       300  // Seconds of anomaly before flagging (5 min)
#define NT_DISCIPLINE_THRESH 1.0f // Drift threshold (sec/hour) to trigger aging adjust
#define NT_STARVATION_SEC    10   // Seconds without pulses = starvation alarm
#define NT_EVENT_LOG_SIZE    24   // NVS event log entries

// --- Checkpoint record ---
struct nt_checkpoint_t {
    uint32_t epoch;       // RTC epoch at checkpoint
    uint32_t counts;      // Total pulse count at checkpoint
    float    temp_c;      // DS3231 temperature at checkpoint
    float    lambda;      // Counts/sec measured this interval
    float    chi2;        // Chi-square statistic for this interval
    bool     chi2_pass;   // Did it pass the goodness-of-fit test?
};

// --- Nuclear time estimate ---
struct nt_time_t {
    float    elapsed_sec;     // Nuclear-derived elapsed time since last checkpoint
    float    uncertainty_sec; // 1-sigma uncertainty on elapsed time
    int32_t  delta_ms;        // Nuclear time - RTC time (ms)
    bool     valid;           // Is the estimate valid?
};

// --- Kalman fused time (step 31) ---
struct nt_fused_t {
    float    offset_sec;      // Fused RTC offset estimate (sec)
    float    uncertainty_sec; // Fused uncertainty (sec)
    float    drift_rate;      // Estimated drift rate (sec/sec)
};

// --- Status flags ---
enum nt_stratum_t {
    NT_STRATUM_UNCAL = 0,  // Not yet calibrated
    NT_STRATUM_CAL,        // Calibrating (collecting data)
    NT_STRATUM_0,          // Nuclear source verified
    NT_STRATUM_1,          // RTC disciplined by nuclear (step 34)
    NT_STRATUM_HOLDOVER    // Source removed, running on RTC (step 32)
};

void           nt_init();
void           nt_update(uint32_t rtc_epoch, uint32_t total_counts, float temp_c);
float          nt_get_lambda();         // Calibrated decay rate (counts/sec)
bool           nt_is_calibrated();
nt_stratum_t   nt_get_stratum();
bool           nt_source_anomaly();     // True if source appears missing/changed
nt_time_t      nt_get_time();           // Current nuclear time estimate
nt_fused_t     nt_get_fused();          // Step 31: Kalman fused time
float          nt_get_drift_ppm();      // Estimated RTC drift in ppm
uint8_t        nt_get_trust();          // Step 33: Trust score 0-100
bool           nt_is_starved();         // Step 36: No pulses for >10s

// Checkpoint access
uint8_t              nt_checkpoint_count();
const nt_checkpoint_t* nt_get_checkpoint(uint8_t idx); // 0 = oldest available

// NVS persistence
bool           nt_save_calibration();
bool           nt_load_calibration();

// Step 35: Event log
uint8_t        nt_event_count();
bool           nt_get_event(uint8_t idx, char* buf, size_t len);

// Force recalibration
void           nt_recalibrate();
