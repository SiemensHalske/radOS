#include "trng.h"
#include "mbedtls/sha256.h"
#include <Arduino.h>

// --- Step 1: Microsecond timestamp ring buffer (ISR-safe) ---
static volatile unsigned long ts_buf[TRNG_TS_BUF_SIZE];
static volatile uint16_t ts_write = 0;
static uint16_t ts_read = 0;

// --- Step 2: Inter-pulse interval state ---
static unsigned long prev_ts = 0;
static bool have_prev = false;

// --- Step 3: Von Neumann debiasing state ---
static uint32_t vn_prev_dt = 0;
static bool vn_have_prev = false;

// --- Step 4: Entropy pool (256-bit) ---
static uint8_t pool[TRNG_POOL_SIZE];
static uint16_t pool_bit_idx = 0;  // Next bit position to XOR into

// --- Step 5: Entropy accounting ---
static volatile uint16_t bits_accumulated = 0;
static uint32_t total_served = 0;

// --- Step 9: Rate tracking ---
static uint32_t rate_bits = 0;
static uint32_t rate_last_ms = 0;
static float current_rate = 0.0f;

// --- Step 47: NIST SP 800-90B health tests ---
// Warmup: skip health test evaluation during HV ramp-up
// (non-stationary intervals → Von Neumann debiaser produces biased runs)
#define TRNG_HEALTH_WARMUP_MS  90000  // 90 seconds for HV to stabilize
static uint32_t health_init_ms = 0;
static bool health_warmup_done = false;

// Repetition Count Test (RCT): detect stuck source
static uint8_t  rct_prev_bit = 2;   // Invalid initial value
static uint16_t rct_count = 0;      // Current run length
static bool     rct_alarm = false;
static uint32_t rct_fail_count = 0;

// Adaptive Proportion Test (APT): detect bias
static uint8_t  apt_base = 0;       // Reference symbol
static uint16_t apt_count = 0;      // Count of base symbol in window
static uint16_t apt_window_pos = 0; // Position in window
static bool     apt_alarm = false;
static bool     apt_first_window = true; // Don't alarm until first window done
static uint32_t apt_fail_count = 0;

// --- Step 48: Continuous self-test ---
static uint32_t selftest_served = 0;     // Bytes since last self-test
static bool     selftest_ok = true;
static uint32_t selftest_fail_count = 0;

// --- Step 1: Called from pulse ISR ---
void trng_feed(unsigned long timestamp_us) {
    uint16_t next = (ts_write + 1) % TRNG_TS_BUF_SIZE;
    if (next != ts_read) {  // Drop if buffer full
        ts_buf[ts_write] = timestamp_us;
        ts_write = next;
    }
}

void trng_init() {
    memset(pool, 0, TRNG_POOL_SIZE);
    pool_bit_idx = 0;
    bits_accumulated = 0;
    total_served = 0;
    ts_write = 0;
    ts_read = 0;
    have_prev = false;
    vn_have_prev = false;
    rate_bits = 0;
    rate_last_ms = millis();
    current_rate = 0.0f;
    // Step 47: Reset health tests
    health_init_ms = millis();
    health_warmup_done = false;
    rct_prev_bit = 2;
    rct_count = 0;
    rct_alarm = false;
    rct_fail_count = 0;
    apt_window_pos = 0;
    apt_count = 0;
    apt_alarm = false;
    apt_first_window = true;
    apt_fail_count = 0;
    // Step 48: Reset self-test
    selftest_served = 0;
    selftest_ok = true;
}

// Step 47: Run NIST SP 800-90B health tests on each debiased bit
static void health_check_bit(uint8_t bit) {
    // Skip evaluation during warmup (HV ramp-up produces non-stationary data)
    if (!health_warmup_done) {
        if ((millis() - health_init_ms) < TRNG_HEALTH_WARMUP_MS) return;
        health_warmup_done = true;
        // Reset RCT state for clean start after warmup
        rct_prev_bit = 2;
        rct_count = 0;
        rct_alarm = false;
    }

    // --- Repetition Count Test ---
    if (bit == rct_prev_bit) {
        rct_count++;
        if (rct_count == TRNG_RCT_CUTOFF && !rct_alarm) {
            rct_alarm = true;
            rct_fail_count++;
        }
    } else {
        rct_prev_bit = bit;
        rct_count = 1;
        rct_alarm = false;  // Self-clearing on new symbol
    }

    // --- Adaptive Proportion Test ---
    if (apt_window_pos == 0) {
        // Start new window
        apt_base = bit;
        apt_count = 1;
        apt_window_pos = 1;
    } else {
        if (bit == apt_base) apt_count++;
        apt_window_pos++;

        // Early alarm: if count already exceeds cutoff
        if (apt_count >= TRNG_APT_CUTOFF && !apt_first_window) {
            apt_alarm = true;
            apt_fail_count++;
            apt_window_pos = 0; // Reset window
        }
        // Window complete
        if (apt_window_pos >= TRNG_APT_WINDOW) {
            apt_first_window = false;
            apt_alarm = false;
            apt_window_pos = 0;
        }
    }
}

// Mix one debiased bit into the entropy pool via XOR folding
static void pool_mix_bit(uint8_t bit) {
    health_check_bit(bit);
    uint16_t byte_idx = (pool_bit_idx / 8) % TRNG_POOL_SIZE;
    uint8_t  bit_pos  = pool_bit_idx % 8;
    pool[byte_idx] ^= (bit << bit_pos);
    pool_bit_idx++;
    bits_accumulated++;
    rate_bits++;
}

// --- Steps 2-3: Process timestamps into debiased bits ---
void trng_process() {
    // Drain timestamp buffer
    while (ts_read != ts_write) {
        unsigned long ts = ts_buf[ts_read];
        ts_read = (ts_read + 1) % TRNG_TS_BUF_SIZE;

        // Step 2: Compute interval
        if (!have_prev) {
            prev_ts = ts;
            have_prev = true;
            continue;
        }
        uint32_t dt = (uint32_t)(ts - prev_ts);

        // Filter out afterpulses: GM tube produces spurious discharges
        // at 100-500 µs post-event. Skip short intervals to keep only
        // genuine Poisson-distributed events for debiasing.
        if (dt < TRNG_MIN_INTERVAL_US) continue;  // keep prev_ts unchanged

        prev_ts = ts;

        // Step 3: Von Neumann debiasing on interval pairs
        if (!vn_have_prev) {
            vn_prev_dt = dt;
            vn_have_prev = true;
            continue;
        }

        // Compare pair: dt1 < dt2 → 1, dt1 > dt2 → 0, equal → discard
        if (vn_prev_dt < dt) {
            pool_mix_bit(1);
        } else if (vn_prev_dt > dt) {
            pool_mix_bit(0);
        }
        // Equal intervals are discarded (extremely rare with µs resolution)
        vn_have_prev = false;
    }

    // Step 9: Update rate every second
    uint32_t now = millis();
    if ((now - rate_last_ms) >= 1000) {
        current_rate = (float)rate_bits * 1000.0f / (float)(now - rate_last_ms);
        rate_bits = 0;
        rate_last_ms = now;
    }
}

// --- Step 5: Bits of entropy available ---
uint16_t trng_available() {
    return bits_accumulated;
}

// --- Step 6: SHA-256 conditioned output ---
bool trng_read(uint8_t *buf, size_t len) {
    if (bits_accumulated < TRNG_POOL_BITS) return false;  // Not enough entropy
    // Step 47: Block output if health tests are failing
    if (rct_alarm || apt_alarm) return false;

    size_t pos = 0;
    while (pos < len) {
        // Hash pool + counter for unique output blocks
        uint8_t hash[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);  // SHA-256 (not 224)
        mbedtls_sha256_update(&ctx, pool, TRNG_POOL_SIZE);
        // Mix in a counter so repeated reads produce different output
        uint32_t ctr = total_served;
        mbedtls_sha256_update(&ctx, (uint8_t *)&ctr, sizeof(ctr));
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);

        // Copy hash bytes to output
        size_t chunk = (len - pos) < 32 ? (len - pos) : 32;
        memcpy(buf + pos, hash, chunk);
        pos += chunk;
        total_served += chunk;

        // Re-seed pool from hash remainder (feedback)
        for (int i = 0; i < TRNG_POOL_SIZE; i++) {
            pool[i] ^= hash[i];
        }
        // Deduct entropy — require re-accumulation
        bits_accumulated = (bits_accumulated > TRNG_POOL_BITS)
                            ? bits_accumulated - TRNG_POOL_BITS : 0;
    }

    // Step 48: Continuous self-test — monobit frequency test every N bytes
    selftest_served += len;
    if (selftest_served >= TRNG_SELFTEST_INTERVAL) {
        selftest_served = 0;
        // Run monobit test on the output we just produced
        uint16_t ones = 0;
        for (size_t i = 0; i < len; i++)
            for (int b = 0; b < 8; b++)
                if (buf[i] & (1 << b)) ones++;
        uint16_t total_bits = len * 8;
        // NIST monobit: |ones - N/2| < threshold
        // For 256 bits: threshold ~= 16 (p=0.01). Scale linearly.
        int16_t deviation = (int16_t)ones - (int16_t)(total_bits / 2);
        if (deviation < 0) deviation = -deviation;
        // Allow ±20% deviation from expected (generous for small samples)
        uint16_t threshold = total_bits / 5;
        if (threshold < 16) threshold = 16;
        if ((uint16_t)deviation > threshold) {
            selftest_ok = false;
            selftest_fail_count++;
        } else {
            selftest_ok = true;
        }
    }

    return true;
}

// --- Step 9: Entropy rate ---
float trng_rate() {
    return current_rate;
}

uint32_t trng_total_bytes() {
    return total_served;
}

// Step 47: Health test accessors
bool trng_health_ok() {
    // During warmup, report OK (not enough data to test)
    if (!health_warmup_done) return true;
    return !rct_alarm && (!apt_alarm || apt_first_window);
}

uint32_t trng_rct_fails() {
    return rct_fail_count;
}

uint32_t trng_apt_fails() {
    return apt_fail_count;
}

// Step 48: Self-test accessors
bool trng_selftest_ok() {
    return selftest_ok;
}

uint32_t trng_selftest_fails() {
    return selftest_fail_count;
}
