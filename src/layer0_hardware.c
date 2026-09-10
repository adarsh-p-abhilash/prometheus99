/**
 * @file layer0_hardware.c
 * @brief Layer 0: Hardware & I/O Abstraction Implementation
 */

#include "../include/layer0_hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

#include <psapi.h>

static uint64_t s_boot_time_ms = 0;
static uint64_t s_last_hardware_pet_ms = 0;
static bool s_hardware_watchdog_tripped = false;
static uint32_t s_isr_counter = 0;

static FILETIME s_prev_idle = {0}, s_prev_kernel = {0}, s_prev_user = {0};
static float s_cached_cpu_pct = 15.0f;
static uint64_t s_last_cpu_sample_ms = 0;
static float s_smoothed_cpu_temp_C = 42.0f;

static uint64_t filetime_to_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | (uint64_t)ft->dwLowDateTime;
}

static float sample_host_cpu_load(uint64_t now_ms)
{
    if (now_ms - s_last_cpu_sample_ms < 100 && s_last_cpu_sample_ms != 0) {
        return s_cached_cpu_pct;
    }

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (s_prev_idle.dwLowDateTime != 0 || s_prev_idle.dwHighDateTime != 0) {
            uint64_t i = filetime_to_u64(&idle) - filetime_to_u64(&s_prev_idle);
            uint64_t k = filetime_to_u64(&kernel) - filetime_to_u64(&s_prev_kernel);
            uint64_t u = filetime_to_u64(&user) - filetime_to_u64(&s_prev_user);
            uint64_t total = k + u;
            if (total > 0) {
                uint64_t busy = total > i ? total - i : 0;
                s_cached_cpu_pct = (float)busy * 100.0f / (float)total;
            }
        }
        s_prev_idle = idle;
        s_prev_kernel = kernel;
        s_prev_user = user;
    }
    s_last_cpu_sample_ms = now_ms;
    return s_cached_cpu_pct;
}

float layer0_get_host_cpu_load(void)
{
    return sample_host_cpu_load(layer0_get_system_time_ms());
}

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
    s_smoothed_cpu_temp_C = 42.0f;
    
    /* Warm up CPU time counters */
    GetSystemTimes(&s_prev_idle, &s_prev_kernel, &s_prev_user);

    printf("[LAYER 0 HAL] Live Host System Hardware Telemetry Initialized.\n");
}

void layer0_read_raw_sensors(raw_hardware_sensors_t *sensors)
{
    if (!sensors) return;

    uint64_t now = layer0_get_system_time_ms();
    s_isr_counter++;

    /* 1. Host CPU Utilization & Thermal Model with Thermal Inertia Smoothing Filter (EMA) */
    float cpu_load_pct = sample_host_cpu_load(now);
    float target_cpu_temp_C = 38.0f + (cpu_load_pct * 0.60f);
    
    /* Apply EMA Thermal Inertia Filter across 1000 Hz ISR Ticks */
    s_smoothed_cpu_temp_C = (s_smoothed_cpu_temp_C * 0.998f) + (target_cpu_temp_C * 0.002f);
    uint32_t raw_temp = (uint32_t)(s_smoothed_cpu_temp_C * 1000.0f); /* milli-Celsius */

    /* 2. Host RAM Physical Memory Utilization % */
    uint32_t raw_ram_pct = 45; /* 45% default */
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_status)) {
        raw_ram_pct = (uint32_t)mem_status.dwMemoryLoad; /* e.g. 52% RAM load */
    }

    /* 3. Host System Thread / Process Activity */
    uint32_t raw_rpm = 2400; /* Default nominal fallback */
    PERFORMANCE_INFORMATION perf_info;
    perf_info.cb = sizeof(PERFORMANCE_INFORMATION);
    if (GetPerformanceInfo(&perf_info, sizeof(perf_info))) {
        raw_rpm = (uint32_t)perf_info.ThreadCount;
    }
    /* Dynamic live thread count update pulse */
    raw_rpm += ((s_isr_counter / 10) % 7);

    /* 4. Host AC Line Power & Battery Bus Voltage (24000 mV nominal AC, or battery voltage) */
    uint32_t raw_v = 24000;
    SYSTEM_POWER_STATUS pwr_status;
    if (GetSystemPowerStatus(&pwr_status)) {
        if (pwr_status.ACLineStatus == 1) {
            raw_v = 24000 + ((s_isr_counter % 20) * 5); /* 24.0V AC stable */
        } else if (pwr_status.BatteryLifePercent != 255) {
            raw_v = 11000 + ((uint32_t)pwr_status.BatteryLifePercent * 130); /* Battery bus */
        }
    }

    sensors->raw_adc_temp = raw_temp;
    sensors->raw_adc_pressure = raw_ram_pct;
    sensors->raw_adc_rpm = raw_rpm;
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
