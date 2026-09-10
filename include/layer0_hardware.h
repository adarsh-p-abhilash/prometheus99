/**
 * @file layer0_hardware.h
 * @brief Layer 0: Hardware & I/O Abstraction Layer
 * 
 * Manages raw hardware interrupts, physical watchdog timer interface,
 * button GPIO pin registers, digitizer touch hardware, and display panel interface.
 */

#ifndef LAYER0_HARDWARE_H
#define LAYER0_HARDWARE_H

#include "config.h"

/*
 * NOTE: the synthetic raw_hardware_sensors_t generator that used to live here
 * has been removed. Telemetry is now real host data produced by the Layer 1
 * metric providers (see layer1_metrics.h); nothing in the runtime fabricates
 * a sensor value any more.
 */

/* --- Raw Hardware Touch Event --- */
typedef struct {
    bool is_pressed;
    int16_t x;
    int16_t y;
} raw_touch_data_t;

/* --- Function Declarations --- */
void layer0_hardware_init(void);
void layer0_read_raw_buttons(uint32_t *raw_gpio_mask);
void layer0_read_raw_touch(raw_touch_data_t *touch);
void layer0_pet_hardware_watchdog(void);
bool layer0_is_hardware_watchdog_tripped(void);
uint64_t layer0_get_system_time_ms(void);

#endif /* LAYER0_HARDWARE_H */
