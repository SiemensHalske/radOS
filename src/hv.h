#pragma once

#include "config.h"

void hv_init();
void hv_update();
bool hv_ready();
uint32_t hv_read_mv();
uint8_t hv_duty();
