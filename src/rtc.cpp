#include "rtc.h"
#include <Wire.h>
#include <RTClib.h>

static RTC_DS3231 rtc;
static bool _rtc_ok = false;

bool rtc_init() {
    Wire.begin(RTC_SDA, RTC_SCL);
    if (!rtc.begin(&Wire)) {
        _rtc_ok = false;
        return false;
    }
    _rtc_ok = true;
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    return true;
}

bool rtc_available() {
    return _rtc_ok;
}

String rtc_timestamp() {
    if (!_rtc_ok) {
        uint32_t s = millis() / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "T+%02d:%02d:%02d", (int)(s/3600), (int)((s/60)%60), (int)(s%60));
        return String(buf);
    }
    DateTime now = rtc.now();
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());
    return String(buf);
}

uint32_t rtc_epoch() {
    if (!_rtc_ok) return millis() / 1000;
    return rtc.now().unixtime();
}

float rtc_temperature() {
    if (!_rtc_ok) return 0.0f;
    return rtc.getTemperature();
}

// Step 30: DS3231 aging register
// Register 0x10, signed 8-bit. Positive = slow crystal, negative = fast crystal.
// Each step ≈ 0.1 ppm change in frequency.
#define DS3231_AGING_REG 0x10
#define DS3231_I2C_ADDR  0x68

int8_t rtc_get_aging() {
    if (!_rtc_ok) return 0;
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(DS3231_AGING_REG);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)DS3231_I2C_ADDR, (uint8_t)1);
    return (int8_t)Wire.read();
}

void rtc_set_aging(int8_t offset) {
    if (!_rtc_ok) return;
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(DS3231_AGING_REG);
    Wire.write((uint8_t)offset);
    Wire.endTransmission();
}

void rtc_adjust_aging(int8_t delta) {
    if (!_rtc_ok) return;
    int16_t current = rtc_get_aging();
    int16_t next = current + delta;
    if (next > 127) next = 127;
    if (next < -128) next = -128;
    rtc_set_aging((int8_t)next);
}
