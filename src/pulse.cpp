#include "pulse.h"
#include "trng.h"
#include <Adafruit_NeoPixel.h>

#define RING_SIZE 60 // 1 bucket per second for 60s window

static Adafruit_NeoPixel neo(1, NEO_PIN, NEO_GRB + NEO_KHZ800);
static volatile uint32_t pulse_count = 0;
static volatile uint32_t total_count = 0;
static volatile uint32_t pending_clicks = 0;
static uint32_t ring[RING_SIZE];
static uint8_t ring_idx = 0;
static uint32_t ring_sum = 0;
static uint32_t last_tick = 0;
static volatile unsigned long last_pulse_us = 0;
static uint32_t neo_off_at = 0;
static uint32_t last_cps = 0;
static uint32_t sec_count = 0;
static uint8_t idle_r = 0, idle_g = 0, idle_b = 0;

static void IRAM_ATTR on_pulse()
{
    unsigned long now = micros();
    if ((now - last_pulse_us) > DEAD_TIME_US)
    {
        pulse_count++;
        total_count++;
        pending_clicks++;
        trng_feed(now);
        last_pulse_us = now;
    }
}

void pulse_init()
{
    pinMode(TRG_PIN, INPUT);
    neo.begin();
    neo.setBrightness(NEO_BRIGHTNESS);
    neo.clear();
    neo.show();
    memset(ring, 0, sizeof(ring));
    attachInterrupt(digitalPinToInterrupt(TRG_PIN), on_pulse, RISING);
    last_tick = millis();
}

// Call this every loop iteration — advances the ring buffer each second
uint32_t pulse_get_cpm()
{
    uint32_t now = millis();

    // Immediate per-pulse feedback
    noInterrupts();
    uint32_t clicks = pending_clicks;
    pending_clicks = 0;
    interrupts();

    if (clicks > 0)
    {
        neo.setPixelColor(0, neo.Color(NEO_R, NEO_G, NEO_B));
        neo.show();
        neo_off_at = now + NEO_FLASH_MS;

        digitalWrite(OUT_PIN, HIGH);
        delayMicroseconds(50);
        digitalWrite(OUT_PIN, LOW);

        sec_count += clicks;
    }

    if ((now - last_tick) >= 1000)
    {
        last_tick += 1000;

        last_cps = sec_count;

        // Update ring buffer
        ring_sum -= ring[ring_idx];
        ring[ring_idx] = sec_count;
        ring_sum += sec_count;
        ring_idx = (ring_idx + 1) % RING_SIZE;

        sec_count = 0;
    }

    // Turn off NeoPixel after flash duration — show idle color
    if (neo_off_at && now >= neo_off_at)
    {
        neo.setPixelColor(0, neo.Color(idle_r, idle_g, idle_b));
        neo.show();
        neo_off_at = 0;
    }

    return ring_sum;
}

// Step 14: Set NeoPixel idle color (shown when not flashing a pulse)
void pulse_set_neo_idle(uint8_t r, uint8_t g, uint8_t b)
{
    idle_r = r;
    idle_g = g;
    idle_b = b;
}

void pulse_set_neo_brightness(uint8_t brightness)
{
    neo.setBrightness(brightness);
}

uint32_t pulse_get_cps()
{
    return last_cps;
}

void pulse_enable_output()
{
    // OUT_PIN already OUTPUT LOW from setup, nothing extra needed
}

uint32_t pulse_get_total()
{
    noInterrupts();
    uint32_t t = total_count;
    interrupts();
    return t;
}
