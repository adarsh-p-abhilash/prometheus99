/**
 * @file layer1_hal.c
 * @brief Layer 1: Hardware Abstraction Layer & Drivers Implementation
 */

#include "../include/layer1_hal.h"
#include "../include/layer0_hardware.h"
#include "../include/layer2_core.h"
#include <stdio.h>

static uint64_t s_last_hw_heartbeat_ms = 0;
static input_key_t s_queued_key = KEY_NONE;
static raw_touch_data_t s_current_touch = {0};

void layer1_hal_init(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
    s_queued_key = KEY_NONE;
    s_current_touch.is_pressed = false;
    printf("[LAYER 1 HAL] Drivers & 1000Hz Sensor ISR initialized.\n");
}

/* Executed by High-Resolution Interrupt / Thread at 1000 Hz */
void layer1_sensor_isr_handler_1000hz(void)
{
    raw_hardware_sensors_t raw;
    layer0_read_raw_sensors(&raw);

    /* Ingest binary telemetry into Layer 2 Shared State Buffer */
    layer2_update_state_binary(
        raw.raw_adc_temp,
        raw.raw_adc_pressure,
        raw.raw_adc_rpm,
        raw.raw_adc_bus_v
    );
}

void layer1_report_hardware_alive(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
}

uint64_t layer1_get_last_hardware_heartbeat_ms(void)
{
    return s_last_hw_heartbeat_ms;
}

void layer1_display_flush_cb(const display_area_t *area, const uint32_t *color_p)
{
    (void)area;
    (void)color_p;
    
    /* Signal Hardware Flush Heartbeat to Layer 2 Watchdog Supervisor */
    layer1_report_hardware_alive();
}

bool layer1_poll_button_event(input_key_t *key_out)
{
    if (s_queued_key != KEY_NONE) {
        if (key_out) *key_out = s_queued_key;
        s_queued_key = KEY_NONE;
        return true;
    }
    return false;
}

bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed)
{
    if (pressed) *pressed = s_current_touch.is_pressed;
    if (x) *x = s_current_touch.x;
    if (y) *y = s_current_touch.y;
    return s_current_touch.is_pressed;
}
