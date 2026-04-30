#include "hv.h"

void hv_init()
{
    ledcAttach(HV_PIN, HV_FREQ, 8); // 8-bit resolution
    ledcWrite(HV_PIN, (uint32_t)(HV_DUTY * 255));
}

void hv_update()
{
    // No software regulation — HV circuit self-regulates
}

bool hv_ready()
{
    return true;
}

uint32_t hv_read_mv()
{
    return 0;
}
