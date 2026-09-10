/**
 * @file layer0_hardware.c
 * @brief Layer 0: Hardware & I/O Abstraction Implementation
 */

#include "../include/layer0_hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

static uint64_t s_boot_time_ms = 0;
static uint64_t s_last_hardware_pet_ms = 0;
static bool s_hardware_watchdog_tripped = false;
static uint32_t s_isr_counter = 0;

uint64_t layer0_get_system_time_ms(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000) / freq.QuadPart);
}

void layer0_hardware_init(void)
{
    s_boot_time_ms = layer0_get_system_time_ms();
    s_last_hardware_pet_ms = s_boot_time_ms;
    s_hardware_watchdog_tripped = false;
    s_isr_counter = 0;
    printf("[LAYER 0 HAL] Hardware I/O initialized. System timer zeroed.\n");
}

void layer0_read_raw_sensors(raw_hardware_sensors_t *sensors)
{
    if (!sensors) return;

    uint64_t now = layer0_get_system_time_ms();
    uint64_t elapsed_sec = (now - s_boot_time_ms) / 1000;
    
    s_isr_counter++;

    /* Simulate dynamic fieldbus sensor readings */
    /* Temp: 40.0 C to 65.0 C oscillation */
    int temp_wave = (int)(elapsed_sec % 30);
    uint32_t raw_temp = 40000 + (temp_wave * 800) + (rand() % 150);

    /* Pressure: 100.0 kPa to 125.0 kPa */
    uint32_t raw_press = 1000 + (temp_wave * 12) + (rand() % 5);

    /* Motor RPM: 1200 RPM to 3500 RPM */
    uint32_t raw_rpm = 1200 + ((temp_wave % 15) * 150) + (rand() % 30);

    /* Fieldbus Voltage: 24.0V nominal (23800 mV to 24200 mV) */
    uint32_t raw_v = 24000 + ((rand() % 400) - 200);

    sensors->raw_adc_temp = raw_temp;
    sensors->raw_adc_pressure = raw_press;
    sensors->raw_adc_rpm = raw_rpm;
    sensors->raw_adc_bus_v = raw_v;
    sensors->raw_adc_bus_v = raw_v;
    sensors->interrupt_counter = s_isr_counter;
}

void layer0_read_raw_buttons(uint32_t *raw_gpio_mask)
{
    if (!raw_gpio_mask) return;
    *raw_gpio_mask = 0;
    /* Hardware GPIO line mock - populated by Windows OS message pump in main driver */
}

void layer0_read_raw_touch(raw_touch_data_t *touch)
{
    if (!touch) return;
    /* Raw digitizer hardware values populated by HAL driver */
}

void layer0_pet_hardware_watchdog(void)
{
    s_last_hardware_pet_ms = layer0_get_system_time_ms();
    s_hardware_watchdog_tripped = false;
}

bool layer0_is_hardware_watchdog_tripped(void)
{
    uint64_t now = layer0_get_system_time_ms();
    if ((now - s_last_hardware_pet_ms) > WATCHDOG_MAX_AGE_MS) {
        s_hardware_watchdog_tripped = true;
    }
    return s_hardware_watchdog_tripped;
}
