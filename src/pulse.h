#pragma once

#include "config.h"

void pulse_init();
void pulse_enable_output();
uint32_t pulse_get_cpm();
uint32_t pulse_get_cps();
uint32_t pulse_get_total();
void pulse_set_neo_idle(uint8_t r, uint8_t g, uint8_t b);
void pulse_set_neo_brightness(uint8_t brightness);
