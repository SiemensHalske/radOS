#pragma once

#include "config.h"

bool rtc_init();
bool rtc_available(); // Step 46: true if RTC hardware responding
String rtc_timestamp();
uint32_t rtc_epoch();    // Unix epoch seconds
float rtc_temperature(); // DS3231 on-chip temperature (°C)

// Step 30: DS3231 aging register (-128..+127, ~0.1 ppm/step)
int8_t rtc_get_aging();
void rtc_set_aging(int8_t offset);
void rtc_adjust_aging(int8_t delta); // Add delta to current aging
