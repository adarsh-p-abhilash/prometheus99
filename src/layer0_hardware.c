/**
 * @file layer0_hardware.c
 * @brief Layer 0: Hardware & I/O Abstraction Implementation
 */

#include "../include/layer0_hardware.h"
#include <stdio.h>
#include <windows.h>

static uint64_t s_last_hardware_pet_ms = 0;
static bool s_hardware_watchdog_tripped = false;

/* QPC frequency is fixed at boot; querying it per call doubled the cost of a
 * helper invoked ~3000x/sec from the 1000 Hz ISR path. */
static LONGLONG s_qpc_freq = 0;

uint64_t layer0_get_system_time_ms(void)
{
    LARGE_INTEGER count;
    if (s_qpc_freq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpc_freq = freq.QuadPart;
    }
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000) / s_qpc_freq);
}

void layer0_hardware_init(void)
{
    s_last_hardware_pet_ms = layer0_get_system_time_ms();
    s_hardware_watchdog_tripped = false;
    printf("[LAYER 0 HAL] Hardware I/O initialized. System timer zeroed.\n");
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
