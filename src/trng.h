#pragma once

#include "config.h"

// --- TRNG configuration ---
#define TRNG_TS_BUF_SIZE 256 // Microsecond timestamp ring buffer size
#define TRNG_POOL_SIZE 32    // Entropy pool size in bytes (256 bits)
#define TRNG_POOL_BITS (TRNG_POOL_SIZE * 8)

// Minimum inter-pulse interval for TRNG (µs). Filters out GM tube afterpulses
// (spurious discharges at 100-500 µs post-event) that create correlated
// interval pairs and bias the Von Neumann debiaser.
#define TRNG_MIN_INTERVAL_US 5000 // 5 ms

// Step 47: NIST SP 800-90B health test parameters
// Repetition Count Test: how many identical consecutive outputs before alarm
// With afterpulse filtering, debiased bits are well-behaved.
// For H≈0.5 bits/sample, alpha=2^-20: C = 1 + ceil(20/0.5) = 41
#define TRNG_RCT_CUTOFF 41
// Adaptive Proportion Test: window size W=1024 for binary source
#define TRNG_APT_WINDOW 1024
// APT cutoff: for H=1, alpha=2^-20, C ≈ W/2 + 4.5*sqrt(W) ≈ 656
#define TRNG_APT_CUTOFF 656

// Step 48: Continuous self-test interval (bytes served between tests)
#define TRNG_SELFTEST_INTERVAL 1000

void trng_init();
void trng_feed(unsigned long timestamp_us); // Called from ISR
void trng_process();                        // Called from main loop
uint16_t trng_available();                  // Debiased bits accumulated
bool trng_read(uint8_t *buf, size_t len);   // Read conditioned random bytes
float trng_rate();                          // Entropy bits per second
uint32_t trng_total_bytes();                // Total bytes served

// Step 47: Health test status
bool trng_health_ok();     // True if all health tests passing
uint32_t trng_rct_fails(); // Cumulative RCT failures
uint32_t trng_apt_fails(); // Cumulative APT failures

// Step 48: Continuous self-test
bool trng_selftest_ok();        // True if last monobit test passed
uint32_t trng_selftest_fails(); // Cumulative self-test failures
