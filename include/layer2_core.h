/**
 * @file layer2_core.h
 * @brief Layer 2: Core State Management, Serialization, FSM & Watchdog Supervisor
 * 
 * Manages the static binary Shared State Buffer, deterministic finite state machine (FSM),
 * and the Dual-Loop AND Check Watchdog Supervisor.
 */

#ifndef LAYER2_CORE_H
#define LAYER2_CORE_H

#include "config.h"

/* --- Binary Packed Shared State Buffer Structure --- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic_header;       /* 0x50524F4D ('PROM') */
    uint64_t timestamp_ms;       /* High-resolution timestamp */
    uint32_t sensor_temp_mC;     /* Temperature in milli-Celsius (e.g. 42500 = 42.50 C) */
    uint32_t sensor_pressure_kPa;/* Pressure in kPa (e.g. 1013 = 101.3 kPa) */
    uint32_t sensor_rpm;         /* Engine/Motor RPM speed */
    uint32_t sensor_bus_mv;      /* System Bus Voltage in mV (e.g. 24000 = 24.00V) */
    uint8_t  alarm_severity;     /* Active alarm level (alarm_level_t) */
    uint8_t  active_screen;      /* Currently active screen ID (screen_id_t) */
    uint8_t  dirty_flag;         /* 1 = New unread telemetry data available */
    uint8_t  failover_active;    /* 1 = Hot Standby Failover engaged */
    uint32_t heartbeat_counter;  /* Telemetry frame counter */
    uint16_t checksum;           /* 16-bit binary payload checksum */
} shared_state_buffer_t;
#pragma pack(pop)

/* --- Watchdog Supervisor Status --- */
typedef struct {
    bool hw_loop_alive;
    bool sw_loop_alive;
    uint64_t last_hw_ping_ms;
    uint64_t last_sw_ping_ms;
    uint32_t total_watchdog_pets;
    uint32_t watchdog_fault_count;
    bool system_healthy;
} watchdog_supervisor_t;

/* --- Core Function Declarations --- */
void layer2_core_init(void);

/* Shared State Management & Binary Serialization */
void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv);
const shared_state_buffer_t* layer2_get_state_buffer(void);
bool layer2_consume_dirty_flag(void);

/* FSM Controller */
screen_id_t layer2_fsm_get_active_screen(void);
bool layer2_fsm_request_screen_change(screen_id_t new_screen);
void layer2_fsm_process_event(input_key_t key_event);

/* Watchdog Supervisor (Dual-Loop AND Check) */
void layer2_watchdog_report_software_alive(void);
void layer2_watchdog_supervisor_tick(void);
const watchdog_supervisor_t* layer2_get_watchdog_status(void);

#endif /* LAYER2_CORE_H */
