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

/* --- Raw Sensor Interface --- */
typedef struct {
    uint32_t raw_adc_temp;      /* Raw ADC value for temperature sensor */
    uint32_t raw_adc_pressure;  /* Raw ADC value for pressure sensor */
    uint32_t raw_adc_rpm;       /* Pulse counter raw for motor RPM */
    uint32_t raw_adc_bus_v;     /* Raw ADC value for fieldbus voltage */
    uint32_t interrupt_counter; /* Total ISR trigger count */
} raw_hardware_sensors_t;

/* --- Raw Hardware Touch Event --- */
typedef struct {
    bool is_pressed;
    int16_t x;
    int16_t y;
} raw_touch_data_t;

/* --- Function Declarations --- */
void layer0_hardware_init(void);
void layer0_read_raw_sensors(raw_hardware_sensors_t *sensors);
void layer0_read_raw_buttons(uint32_t *raw_gpio_mask);
void layer0_read_raw_touch(raw_touch_data_t *touch);
void layer0_pet_hardware_watchdog(void);
bool layer0_is_hardware_watchdog_tripped(void);
uint64_t layer0_get_system_time_ms(void);

#endif /* LAYER0_HARDWARE_H */
