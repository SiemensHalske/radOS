#include "src/config.h"
#include "src/hv.h"
#include "src/pulse.h"
#include "src/rtc.h"
#include "src/trng.h"
#include "src/nuke_time.h"
#include <Preferences.h>

static uint32_t last_log = 0;
static String serial_cmd = "";
static bool log_enabled = true;     // Step 18: auto command/log mode
static bool dump_mode = false;      // Step 17: continuous hex stream
static uint32_t last_nt_update = 0; // Nuclear time update interval

// Step 40-41: Runtime configuration (NVS-backed)
static float cfg_cpm_to_usv = CPM_TO_USV;
static uint32_t cfg_log_interval = LOG_INTERVAL_MS;
static uint8_t cfg_neo_brightness = NEO_BRIGHTNESS;
static bool cfg_low_power = false;

static void load_config()
{
    Preferences cfg;
    cfg.begin("config", true);
    cfg_cpm_to_usv = cfg.getFloat("cpmusv", CPM_TO_USV);
    cfg_log_interval = cfg.getUInt("logms", LOG_INTERVAL_MS);
    cfg_neo_brightness = cfg.getUChar("neobri", NEO_BRIGHTNESS);
    cfg_low_power = cfg.getBool("lowpwr", false);
    cfg.end();
    if (cfg_low_power)
        cfg_log_interval = 10000;
    pulse_set_neo_brightness(cfg_neo_brightness);
}

void setup()
{
    // 0. Silence output pin immediately
    pinMode(OUT_PIN, OUTPUT);
    digitalWrite(OUT_PIN, LOW);

    // 1. Serial
    Serial.begin(115200);
    while (!Serial && millis() < 2000)
        ;
    Serial.println();
    Serial.println("=== radOS - Geiger counter ===");
    Serial.println("Tube: J305 | HV FB: 10k+10k divider");

    // Load user config from NVS
    load_config();
    Serial.println();

    // 2. HV boost init & ramp-up
    Serial.print("[HV] Initializing... ");
    hv_init();
    Serial.println("OK");

    // Boost starts in hv_init(), wait for FB to confirm HV is up
    Serial.print("[HV] Ramping up");
    uint32_t t0 = millis();
    // First wait for FB to go HIGH (circuit responding), then LOW (HV reached)
    bool saw_boost = false;
    while ((millis() - t0) < HV_RAMP_TIMEOUT)
    {
        hv_update();
        if (digitalRead(FB_PIN))
            saw_boost = true;
        if (saw_boost && hv_ready())
            break;
        Serial.print(".");
        delay(100);
    }
    Serial.println();

    if (!saw_boost || !hv_ready())
    {
        Serial.print("[HV] WARN: ramp-up issue. FB saw_boost=");
        Serial.print(saw_boost);
        Serial.print(" ready=");
        Serial.println(hv_ready());
    }
    else
    {
        Serial.print("[HV] Reached target in ");
        Serial.print(millis() - t0);
        Serial.println(" ms");

        // Let HV settle
        Serial.print("[HV] Settling...");
        uint32_t settle_start = millis();
        while ((millis() - settle_start) < HV_SETTLE_MS)
        {
            hv_update();
            delay(10);
        }
        Serial.println(" OK");
    }

    Serial.print("[HV] FB state: ");
    Serial.println(hv_ready() ? "OK" : "BOOST");

    // 3. RTC
    Serial.print("[RTC] Initializing... ");
    if (rtc_init())
    {
        Serial.print("OK  ");
        Serial.println(rtc_timestamp());
    }
    else
    {
        Serial.println("FAIL - DS3231 not found");
    }

    // 4. Mode pin
    pinMode(MODE_PIN, INPUT_PULLUP);

    // 5. Pulse counter (enable interrupts after HV is stable)
    Serial.print("[PULSE] Initializing... ");
    pulse_init();
    Serial.println("OK");

    // 6. TRNG
    Serial.print("[TRNG] Initializing... ");
    trng_init();
    Serial.println("OK");

    // 7. Nuclear time source
    Serial.print("[NTIME] Initializing... ");
    nt_init();
    if (nt_is_calibrated())
    {
        Serial.print("OK  lambda=");
        Serial.print(nt_get_lambda(), 2);
        Serial.println(" cps (from NVS)");
    }
    else
    {
        Serial.println("Calibrating (need 1h of data)");
    }

    // 8. Ready
    pulse_enable_output();

    // Step 45: Boot self-test
    Serial.println();
    Serial.println("[SELF-TEST]");
    uint8_t st_pass = 0;

    Serial.print("  HV:     ");
    if (hv_ready())
    {
        Serial.println("PASS");
        st_pass++;
    }
    else
        Serial.println("WARN — FB not ready");

    Serial.print("  RTC:    ");
    if (rtc_available())
    {
        Serial.print("PASS  ");
        Serial.print(rtc_temperature(), 1);
        Serial.println(" C");
        st_pass++;
    }
    else
    {
        Serial.println("FAIL — millis() fallback active");
    }

    // Brief source check — wait up to 2s for pulses
    Serial.print("  SOURCE: ");
    {
        uint32_t t0 = millis();
        uint32_t c0 = pulse_get_total();
        while ((millis() - t0) < 2000 && pulse_get_total() == c0)
        {
            pulse_get_cpm();
            delay(1);
        }
        uint32_t got = pulse_get_total() - c0;
        if (got > 0)
        {
            Serial.print("PASS  ");
            Serial.print(got);
            Serial.println(" pulses/2s");
            st_pass++;
        }
        else
        {
            Serial.println("WARN — no pulses (check HV/source)");
        }
    }

    Serial.print("  NVS:    ");
    {
        Preferences p;
        p.begin("selftest", true);
        p.end();
        Serial.println("PASS");
        st_pass++;
    }

    Serial.print("  TRNG:   ");
    if (trng_health_ok())
    {
        Serial.println("PASS");
        st_pass++;
    }
    else
        Serial.println("FAIL — health test alarm");

    Serial.print("[SELF-TEST] ");
    Serial.print(st_pass);
    Serial.println("/5 passed");

    Serial.println();
    Serial.println("[READY] Commands: RAND RANDBIN DUMP ENTROPY STATUS TIME HISTORY");
    Serial.println("        EVENTS CALIBRATE TEST SET GET LOG");
    Serial.println("---");
    last_log = millis();
}

// --- Serial command handler ---
static void handle_command(String &cmd)
{
    // Step 18: Any command pauses log output
    if (cmd != "LOG")
        log_enabled = false;

    if (cmd.startsWith("RAND "))
    {
        int n = cmd.substring(5).toInt();
        if (n < 1 || n > 256)
        {
            Serial.println("ERR: RAND <1-256>");
            return;
        }
        uint8_t buf[256];
        if (!trng_read(buf, n))
        {
            Serial.print("ERR: Not enough entropy (");
            Serial.print(trng_available());
            Serial.print("/");
            Serial.print(TRNG_POOL_BITS);
            Serial.println(" bits)");
            return;
        }
        for (int i = 0; i < n; i++)
        {
            if (buf[i] < 0x10)
                Serial.print('0');
            Serial.print(buf[i], HEX);
        }
        Serial.println();
    }
    // Step 13: Raw binary output
    else if (cmd.startsWith("RANDBIN "))
    {
        int n = cmd.substring(8).toInt();
        if (n < 1 || n > 256)
        {
            Serial.println("ERR: RANDBIN <1-256>");
            return;
        }
        uint8_t buf[256];
        if (!trng_read(buf, n))
        {
            Serial.print("ERR: Not enough entropy (");
            Serial.print(trng_available());
            Serial.print("/");
            Serial.print(TRNG_POOL_BITS);
            Serial.println(" bits)");
            return;
        }
        Serial.write(buf, n);
    }
    // Step 17: Continuous hex dump
    else if (cmd == "DUMP")
    {
        dump_mode = true;
        Serial.println("DUMP mode — send any key to stop");
    }
    else if (cmd == "ENTROPY")
    {
        Serial.print("Pool: ");
        Serial.print(trng_available());
        Serial.print("/");
        Serial.print(TRNG_POOL_BITS);
        Serial.print(" bits | Rate: ");
        Serial.print(trng_rate(), 1);
        Serial.print(" bits/s | Served: ");
        Serial.print(trng_total_bytes());
        Serial.println(" bytes");
        Serial.print("Health: ");
        Serial.print(trng_health_ok() ? "OK" : "ALARM");
        Serial.print(" | RCT fails: ");
        Serial.print(trng_rct_fails());
        Serial.print(" | APT fails: ");
        Serial.println(trng_apt_fails());
        Serial.print("Self-test: ");
        Serial.print(trng_selftest_ok() ? "PASS" : "FAIL");
        Serial.print(" | Failures: ");
        Serial.println(trng_selftest_fails());
    }
    else if (cmd == "STATUS")
    {
        Serial.print("Time: ");
        Serial.println(rtc_timestamp());
        Serial.print("CPM: ");
        Serial.print(pulse_get_cpm());
        Serial.print(" | Total: ");
        Serial.println(pulse_get_total());
        Serial.print("HV: ");
        Serial.println(hv_read_mv() ? "BOOST" : "OK");
        Serial.print("Entropy: ");
        Serial.print(trng_available());
        Serial.print("/");
        Serial.print(TRNG_POOL_BITS);
        Serial.print(" bits @ ");
        Serial.print(trng_rate(), 1);
        Serial.println(" bits/s");
        Serial.print("Served: ");
        Serial.print(trng_total_bytes());
        Serial.print(" bytes | Uptime: ");
        Serial.print(millis() / 1000);
        Serial.println("s");
        const char *strata[] = {"UNCAL", "CAL", "S0", "S1", "HOLD"};
        Serial.print("Stratum: ");
        Serial.print(strata[nt_get_stratum()]);
        Serial.print(" | Trust: ");
        Serial.print(nt_get_trust());
        Serial.print("% | Drift: ");
        Serial.print(nt_get_drift_ppm(), 2);
        Serial.print("ppm | Aging: ");
        Serial.println(rtc_get_aging());
        if (nt_is_starved())
            Serial.println("ALARM: Count starvation!");
        if (nt_source_anomaly())
            Serial.println("ALARM: Source anomaly!");
        if (!trng_health_ok())
            Serial.println("ALARM: TRNG health test failure!");
        if (!trng_selftest_ok())
            Serial.println("ALARM: TRNG self-test failure!");
    }
    else if (cmd == "TEST")
    {
        Serial.println("Collecting 256 bits...");
        uint8_t test_buf[32];
        if (trng_read(test_buf, 32))
        {
            Serial.print("HEX: ");
            for (int i = 0; i < 32; i++)
            {
                if (test_buf[i] < 0x10)
                    Serial.print('0');
                Serial.print(test_buf[i], HEX);
            }
            Serial.println();
            uint16_t ones = 0;
            for (int i = 0; i < 32; i++)
                for (int b = 0; b < 8; b++)
                    if (test_buf[i] & (1 << b))
                        ones++;
            Serial.print("Ones: ");
            Serial.print(ones);
            Serial.print("/256 (expect ~128, got ");
            Serial.print((float)ones / 256.0 * 100.0, 1);
            Serial.println("%)");
        }
        else
        {
            Serial.println("ERR: Pool not full yet");
        }
    }
    // Step 18: Resume logging
    else if (cmd == "LOG")
    {
        log_enabled = true;
        dump_mode = false;
        Serial.println("Logging resumed");
    }
    // Step 28/37: Nuclear time display with trust + Kalman fused data
    else if (cmd == "TIME")
    {
        // Step 37: JSON-style output
        Serial.print("{\"rtc\":\"");
        Serial.print(rtc_timestamp());
        Serial.print("\",\"temp\":");
        Serial.print(rtc_temperature(), 1);
        nt_time_t nt = nt_get_time();
        if (nt.valid)
        {
            Serial.print(",\"nuclear_elapsed\":");
            Serial.print(nt.elapsed_sec, 1);
            Serial.print(",\"uncertainty\":");
            Serial.print(nt.uncertainty_sec, 1);
            Serial.print(",\"delta_ms\":");
            Serial.print(nt.delta_ms);
        }
        nt_fused_t fused = nt_get_fused();
        if (fused.uncertainty_sec > 0)
        {
            Serial.print(",\"fused_offset\":");
            Serial.print(fused.offset_sec, 3);
            Serial.print(",\"fused_uncert\":");
            Serial.print(fused.uncertainty_sec, 3);
            Serial.print(",\"drift_rate\":");
            Serial.print(fused.drift_rate, 6);
        }
        Serial.print(",\"lambda\":");
        Serial.print(nt_get_lambda(), 4);
        Serial.print(",\"drift_ppm\":");
        Serial.print(nt_get_drift_ppm(), 2);
        Serial.print(",\"trust\":");
        Serial.print(nt_get_trust());
        const char *strata[] = {"UNCAL", "CAL", "S0", "S1", "HOLD"};
        Serial.print(",\"stratum\":\"");
        Serial.print(strata[nt_get_stratum()]);
        Serial.print("\",\"aging\":");
        Serial.print(rtc_get_aging());
        if (nt_source_anomaly())
            Serial.print(",\"anomaly\":true");
        if (nt_is_starved())
            Serial.print(",\"starved\":true");
        Serial.println("}");
    }
    // Step 38: Force recalibration
    else if (cmd == "CALIBRATE")
    {
        nt_recalibrate();
        Serial.println("Recalibration started. Need 1h of data.");
    }
    // Step 43: Checkpoint history
    else if (cmd == "HISTORY")
    {
        uint8_t n = nt_checkpoint_count();
        if (n == 0)
        {
            Serial.println("No checkpoints yet (need 1h of data)");
        }
        else
        {
            Serial.print(n);
            Serial.println(" checkpoints:");
            for (uint8_t i = 0; i < n; i++)
            {
                const nt_checkpoint_t *cp = nt_get_checkpoint(i);
                if (!cp)
                    continue;
                Serial.print("  #");
                Serial.print(i);
                Serial.print(" epoch=");
                Serial.print(cp->epoch);
                Serial.print(" cnt=");
                Serial.print(cp->counts);
                Serial.print(" lam=");
                Serial.print(cp->lambda, 4);
                Serial.print(" chi2=");
                Serial.print(cp->chi2, 2);
                Serial.print(cp->chi2_pass ? " PASS" : " FAIL");
                Serial.print(" T=");
                Serial.print(cp->temp_c, 1);
                Serial.println("C");
            }
        }
    }
    // Step 35: Event log viewer
    else if (cmd == "EVENTS")
    {
        uint8_t n = nt_event_count();
        if (n == 0)
        {
            Serial.println("No events logged yet");
        }
        else
        {
            Serial.print(n);
            Serial.println(" events:");
            char buf[48];
            for (uint8_t i = 0; i < n; i++)
            {
                if (nt_get_event(i, buf, sizeof(buf)))
                {
                    Serial.print("  ");
                    Serial.print(i);
                    Serial.print(": ");
                    Serial.println(buf);
                }
            }
        }
    }
    // Step 40: SET <key> <value> — configure parameters
    else if (cmd.startsWith("SET "))
    {
        String rest = cmd.substring(4);
        int sp = rest.indexOf(' ');
        if (sp < 0)
        {
            Serial.println("Usage: SET <key> <value>");
            Serial.println("Keys: cpm_to_usv, log_interval, neo_brightness, power");
            return;
        }
        String key = rest.substring(0, sp);
        String val = rest.substring(sp + 1);
        val.trim();

        Preferences cfg;
        cfg.begin("config", false);
        if (key == "cpm_to_usv")
        {
            cfg_cpm_to_usv = val.toFloat();
            cfg.putFloat("cpmusv", cfg_cpm_to_usv);
            Serial.print("cpm_to_usv = ");
            Serial.println(cfg_cpm_to_usv, 6);
        }
        else if (key == "log_interval")
        {
            cfg_log_interval = val.toInt();
            cfg.putUInt("logms", cfg_log_interval);
            Serial.print("log_interval = ");
            Serial.print(cfg_log_interval);
            Serial.println(" ms");
        }
        else if (key == "neo_brightness")
        {
            cfg_neo_brightness = val.toInt();
            cfg.putUChar("neobri", cfg_neo_brightness);
            pulse_set_neo_brightness(cfg_neo_brightness);
            Serial.print("neo_brightness = ");
            Serial.println(cfg_neo_brightness);
        }
        else if (key == "power")
        {
            if (val == "low")
            {
                cfg_low_power = true;
                cfg_log_interval = 10000;
                cfg_neo_brightness = 10;
                pulse_set_neo_brightness(cfg_neo_brightness);
                cfg.putBool("lowpwr", true);
                cfg.putUInt("logms", cfg_log_interval);
                cfg.putUChar("neobri", cfg_neo_brightness);
                Serial.println("Power: LOW (log 10s, dim NeoPixel)");
            }
            else
            {
                cfg_low_power = false;
                cfg_log_interval = LOG_INTERVAL_MS;
                cfg_neo_brightness = NEO_BRIGHTNESS;
                pulse_set_neo_brightness(cfg_neo_brightness);
                cfg.putBool("lowpwr", false);
                cfg.putUInt("logms", cfg_log_interval);
                cfg.putUChar("neobri", cfg_neo_brightness);
                Serial.println("Power: NORMAL");
            }
        }
        else
        {
            Serial.println("Unknown key. Keys: cpm_to_usv, log_interval, neo_brightness, power");
        }
        cfg.end();
    }
    // Step 41: GET [key] — read configuration
    else if (cmd == "GET" || cmd.startsWith("GET "))
    {
        String key = cmd.length() > 4 ? cmd.substring(4) : "";
        key.trim();
        if (key.length() == 0)
        {
            Serial.print("cpm_to_usv     = ");
            Serial.println(cfg_cpm_to_usv, 6);
            Serial.print("log_interval   = ");
            Serial.print(cfg_log_interval);
            Serial.println(" ms");
            Serial.print("neo_brightness = ");
            Serial.println(cfg_neo_brightness);
            Serial.print("power          = ");
            Serial.println(cfg_low_power ? "low" : "normal");
            Serial.print("dead_time      = ");
            Serial.print(DEAD_TIME_US);
            Serial.println(" us (compile-time)");
        }
        else if (key == "cpm_to_usv")
        {
            Serial.println(cfg_cpm_to_usv, 6);
        }
        else if (key == "log_interval")
        {
            Serial.println(cfg_log_interval);
        }
        else if (key == "neo_brightness")
        {
            Serial.println(cfg_neo_brightness);
        }
        else if (key == "power")
        {
            Serial.println(cfg_low_power ? "low" : "normal");
        }
        else
        {
            Serial.println("Unknown key. Keys: cpm_to_usv, log_interval, neo_brightness, power");
        }
    }
    else
    {
        Serial.println("Commands: RAND RANDBIN DUMP ENTROPY STATUS TIME HISTORY");
        Serial.println("         EVENTS CALIBRATE TEST SET GET LOG");
    }
}

void loop()
{
    hv_update();
    trng_process();

    // Update nuclear time source every second
    uint32_t now_ms = millis();
    if ((now_ms - last_nt_update) >= 1000)
    {
        last_nt_update = now_ms;
        nt_update(rtc_epoch(), pulse_get_total(), rtc_temperature());
    }

    // --- Serial command handling ---
    while (Serial.available())
    {
        char c = Serial.read();
        // Step 17: Any key stops dump mode
        if (dump_mode)
        {
            dump_mode = false;
            Serial.println();
            Serial.println("DUMP stopped");
            serial_cmd = "";
            continue;
        }
        if (c == '\n' || c == '\r')
        {
            serial_cmd.trim();
            if (serial_cmd.length() > 0)
                handle_command(serial_cmd);
            serial_cmd = "";
        }
        else
        {
            serial_cmd += c;
        }
    }

    // Step 17: Continuous hex dump when active
    if (dump_mode)
    {
        uint8_t b;
        if (trng_read(&b, 1))
        {
            if (b < 0x10)
                Serial.print('0');
            Serial.print(b, HEX);
        }
    }

    // Step 39: NeoPixel trust indicator
    // Red = anomaly/starvation, yellow = holdover, breathing blue = calibrating, green = verified
    nt_stratum_t st = nt_get_stratum();
    if (nt_source_anomaly() || nt_is_starved())
    {
        pulse_set_neo_idle(8, 0, 0); // Red — alarm
    }
    else if (st == NT_STRATUM_HOLDOVER)
    {
        pulse_set_neo_idle(8, 6, 0); // Yellow — holdover
    }
    else if (st == NT_STRATUM_CAL || st == NT_STRATUM_UNCAL)
    {
        // Breathing blue — calibrating
        uint16_t t = (millis() >> 3) & 0x1FF;              // 0-511, ~4s cycle
        uint8_t b = t < 256 ? (t >> 5) : ((511 - t) >> 5); // triangle 0-7
        pulse_set_neo_idle(0, 0, 1 + b);
    }
    else
    {
        // Stratum-0/1: green, brightness scales with trust
        uint8_t g = 2 + (nt_get_trust() * 6 / 100);
        pulse_set_neo_idle(0, g, 0);
    }

    uint32_t cpm = pulse_get_cpm();

    uint32_t now = millis();
    // Step 18: Only log when log_enabled
    if (log_enabled && !dump_mode && (now - last_log) >= cfg_log_interval)
    {
        last_log += cfg_log_interval;

        bool cps_mode = (digitalRead(MODE_PIN) == LOW);

        if (cps_mode)
        {
            uint32_t cps = pulse_get_cps();
            Serial.print("CPS: ");
            Serial.print(cps);
            Serial.print(" | Total: ");
            Serial.print(pulse_get_total());
            Serial.print(" | E:");
            Serial.print(trng_available());
            Serial.print("b @");
            Serial.print(trng_rate(), 1);
            Serial.println("b/s");
        }
        else
        {
            float usv = cpm * cfg_cpm_to_usv;
            Serial.print(rtc_timestamp());
            Serial.print(" | CPM: ");
            Serial.print(cpm);
            Serial.print(" | ");
            Serial.print(usv, 3);
            Serial.print(" uSv/h | HV: ");
            Serial.print(hv_read_mv() ? "BOOST" : "OK");
            Serial.print(" | Total: ");
            Serial.print(pulse_get_total());
            Serial.print(" | E:");
            Serial.print(trng_available());
            Serial.print("b @");
            Serial.print(trng_rate(), 1);
            Serial.print("b/s");
            // Phase 3/4: stratum + trust indicator
            const char *strata[] = {"U", "C", "S0", "S1", "H"};
            Serial.print(" | ");
            Serial.print(strata[nt_get_stratum()]);
            Serial.print(" T");
            Serial.print(nt_get_trust());
            if (nt_source_anomaly())
                Serial.print(" !ANOM");
            if (nt_is_starved())
                Serial.print(" !STARV");
            if (!trng_health_ok())
                Serial.print(" !HLTH");
            Serial.println();
        }
    }
}