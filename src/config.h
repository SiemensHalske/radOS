#pragma once

#include <Arduino.h>

// --- Pin definitions ---
#define TRG_PIN 6   // GM tube pulse input (interrupt)
#define OUT_PIN 7   // Click output (buzzer/LED)
#define HV_PIN 4    // HV boost converter PWM
#define FB_PIN 0    // HV feedback (ADC)
#define NEO_PIN 8   // Onboard NeoPixel LED
#define MODE_PIN 13 // CPS mode select (LOW = CPS, HIGH/float = CPM)
#define RTC_SDA 12  // DS3231 I2C data
#define RTC_SCL 11  // DS3231 I2C clock

// --- NeoPixel ---
#define NEO_BRIGHTNESS 40 // 0-255, keep low to avoid blinding
#define NEO_R 255         // Warm white/yellow pulse color
#define NEO_G 180
#define NEO_B 50
#define NEO_FLASH_MS 30 // Flash duration (ms)

// --- HV boost converter ---
#define HV_HIGH_TIME 0.0457  // 45.7us high
#define HV_LOW_TIME 0.0229   // 22.9us low
#define HV_FREQ 14545.4545   // PWM frequency (Hz)
#define HV_FB_THRESH 180     // FB target in mV (regulation setpoint)
#define HV_FB_HYST 60        // FB hysteresis in mV (deadband around setpoint)
#define HV_DUTY 0.667        // Initial duty cycle (high / period)
#define HV_DUTY_MIN 0.40     // Lower bound on regulator duty
#define HV_DUTY_MAX 0.85     // Upper bound on regulator duty
#define HV_DUTY_STEP 1       // PWM counts per regulation step (8-bit)
#define HV_REG_INTERVAL_MS 20 // Min interval between regulation steps (ms)
#define HV_RAMP_TIMEOUT 5000 // Max time to wait for HV ramp-up (ms)
#define HV_SETTLE_MS 500     // Settle time after HV reaches target (ms)

// --- J305 GM tube ---
#define CPM_TO_USV 0.00812 // J305 conversion factor: CPM -> uSv/h
#define DEAD_TIME_US 90    // J305 dead time ~90us

// --- Measurement ---
#define LOG_INTERVAL_MS 1000 // Serial output interval (ms)
#define CPM_WINDOW_MS 60000  // Sliding window for CPM (60s)