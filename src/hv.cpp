#include "hv.h"

static uint8_t s_duty = (uint8_t)(HV_DUTY * 255);
static const uint8_t s_duty_min = (uint8_t)(HV_DUTY_MIN * 255);
static const uint8_t s_duty_max = (uint8_t)(HV_DUTY_MAX * 255);
static uint32_t s_last_reg_ms = 0;

void hv_init()
{
    ledcAttach(HV_PIN, HV_FREQ, 8); // 8-bit resolution
    ledcWrite(HV_PIN, s_duty);

    // FB on ADC: 10k+10k divider from 5V FB rail -> 0..~2.5V at the pin.
    // 11dB attenuation gives ~0..3.1V usable range, plenty of headroom.
    analogSetPinAttenuation(FB_PIN, ADC_11db);
    analogReadResolution(12);
}

// Average a few samples to suppress ADC noise.
uint32_t hv_read_mv()
{
    constexpr int N = 8;
    uint32_t acc = 0;
    for (int i = 0; i < N; ++i)
        acc += analogReadMilliVolts(FB_PIN);
    return acc / N;
}

bool hv_ready()
{
    return hv_read_mv() >= HV_FB_THRESH;
}

// Bang-bang with hysteresis and rate limit.
// FB rises with duty (more drive -> NE555 keeps boosting harder),
// so: below setpoint -> increase duty, above setpoint -> decrease.
// Saturated at [HV_DUTY_MIN, HV_DUTY_MAX] to keep NE555 in a sane regime.
void hv_update()
{
    uint32_t now = millis();
    if (now - s_last_reg_ms < HV_REG_INTERVAL_MS)
        return;
    s_last_reg_ms = now;

    uint32_t mv = hv_read_mv();
    uint8_t prev = s_duty;

    if (mv + HV_FB_HYST < HV_FB_THRESH)
    {
        if (s_duty <= s_duty_max - HV_DUTY_STEP)
            s_duty += HV_DUTY_STEP;
        else
            s_duty = s_duty_max;
    }
    else if (mv > HV_FB_THRESH + HV_FB_HYST)
    {
        if (s_duty >= s_duty_min + HV_DUTY_STEP)
            s_duty -= HV_DUTY_STEP;
        else
            s_duty = s_duty_min;
    }

    if (s_duty != prev)
        ledcWrite(HV_PIN, s_duty);
}

uint8_t hv_duty()
{
    return s_duty;
}
