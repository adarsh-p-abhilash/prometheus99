/**
 * @file layer1_hal.h
 * @brief Layer 1: Hardware Abstraction Layer & Drivers
 * 
 * Provides debounced button/touch drivers, 1000 Hz high-speed Sensor ISR,
 * and the Display Flush Callback that signals hardware liveness to Layer 2.
 */

#ifndef LAYER1_HAL_H
#define LAYER1_HAL_H

#include "config.h"

/* --- Display Flush Buffer Structure (8bpp palette indices) --- */
typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
    const uint8_t *pixel_color_p;
} display_area_t;

/* --- Function Declarations --- */
void layer1_hal_init(void);

/* Sensor ISR triggered at 1000 Hz */
void layer1_sensor_isr_handler_1000hz(void);

/*
 * Driver interfaces. Raw events are queued by the platform input driver and
 * drained once per UI tick, so the presentation layer is the single consumer
 * and a burst inside one 33 ms frame cannot be lost.
 */
void layer1_queue_button_event(input_key_t key);
void layer1_queue_touch_event(int16_t x, int16_t y, bool pressed);
bool layer1_poll_button_event(input_key_t *key_out);
bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed);

/* Display Flush Callback (called by LVGL rendering engine) */
void layer1_display_flush_cb(const display_area_t *area, const uint8_t *color_p);

/* Heartbeat reporter from hardware side */
void layer1_report_hardware_alive(void);
uint64_t layer1_get_last_hardware_heartbeat_ms(void);

#endif /* LAYER1_HAL_H */
