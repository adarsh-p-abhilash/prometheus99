/**
 * @file layer1_hal.c
 * @brief Layer 1: Hardware Abstraction Layer & Drivers Implementation
 */

#include "../include/layer1_hal.h"
#include "../include/layer0_hardware.h"
#include "../include/layer2_core.h"
#include <stdio.h>

static uint64_t s_last_hw_heartbeat_ms = 0;

/* Statically sized lock-free-by-construction event queues (single producer and
 * single consumer, both on the UI thread). Power-of-two size for cheap wrap. */
#define INPUT_QUEUE_SIZE 16
#define INPUT_QUEUE_MASK (INPUT_QUEUE_SIZE - 1)

static input_key_t s_key_queue[INPUT_QUEUE_SIZE];
static unsigned s_key_head, s_key_tail;

static raw_touch_data_t s_touch_queue[INPUT_QUEUE_SIZE];
static unsigned s_touch_head, s_touch_tail;

void layer1_hal_init(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
    s_key_head = s_key_tail = 0;
    s_touch_head = s_touch_tail = 0;
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

    /*
     * Heartbeat for the "hardware" side of the dual-loop watchdog. This must be
     * driven by the 1000 Hz ISR thread itself so the HW loop and the 30 Hz SW
     * loop are genuinely independent liveness signals.
     */
    layer1_report_hardware_alive();
}

void layer1_report_hardware_alive(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
}

uint64_t layer1_get_last_hardware_heartbeat_ms(void)
{
    return s_last_hw_heartbeat_ms;
}

void layer1_display_flush_cb(const display_area_t *area, const uint8_t *color_p)
{
    (void)area;
    (void)color_p;

    /*
     * Display flush completion. The hardware-liveness heartbeat is NOT reported
     * here: this callback runs on the 30 Hz presentation thread, so using it as
     * the HW heartbeat would make both watchdog loops depend on the same thread.
     * The real HW heartbeat is emitted from layer1_sensor_isr_handler_1000hz().
     */
}

void layer1_queue_button_event(input_key_t key)
{
    if (key == KEY_NONE) return;
    unsigned next = (s_key_head + 1u) & INPUT_QUEUE_MASK;
    if (next == s_key_tail) return; /* full: drop oldest-wins, never block */
    s_key_queue[s_key_head] = key;
    s_key_head = next;
}

bool layer1_poll_button_event(input_key_t *key_out)
{
    if (s_key_tail == s_key_head) return false;
    if (key_out) *key_out = s_key_queue[s_key_tail];
    s_key_tail = (s_key_tail + 1u) & INPUT_QUEUE_MASK;
    return true;
}

void layer1_queue_touch_event(int16_t x, int16_t y, bool pressed)
{
    unsigned next = (s_touch_head + 1u) & INPUT_QUEUE_MASK;
    if (next == s_touch_tail) return;
    s_touch_queue[s_touch_head].x = x;
    s_touch_queue[s_touch_head].y = y;
    s_touch_queue[s_touch_head].is_pressed = pressed;
    s_touch_head = next;
}

bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed)
{
    if (s_touch_tail == s_touch_head) return false;
    const raw_touch_data_t *t = &s_touch_queue[s_touch_tail];
    if (x) *x = t->x;
    if (y) *y = t->y;
    if (pressed) *pressed = t->is_pressed;
    s_touch_tail = (s_touch_tail + 1u) & INPUT_QUEUE_MASK;
    return true;
}
