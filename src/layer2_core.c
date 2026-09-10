/**
 * @file layer2_core.c
 * @brief Layer 2: Core State Management, Binary Serialization, FSM & Watchdog Implementation
 */

#include "../include/layer2_core.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* Static pre-allocated Shared State Buffer (Zero Heap Allocation) */
static shared_state_buffer_t s_shared_state_buffer;
static watchdog_supervisor_t s_watchdog_supervisor;

/*
 * The Shared State Buffer is written by the 1000 Hz sensor ISR thread and read
 * by the 30 Hz presentation thread. This lock keeps each reader/writer from
 * observing a half-updated packet (torn 64-bit timestamp, mismatched fields).
 */
static CRITICAL_SECTION s_state_lock;
static bool s_state_lock_ready = false;

/* Static FSM state */
static screen_id_t s_active_screen = SCREEN_BOOT;
static screen_id_t s_previous_screen = SCREEN_BOOT;

/*
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the packed payload.
 * Previously a plain additive byte sum labelled "CRC16" in the UI, which would
 * not detect byte reordering or compensating errors.
 */
static uint16_t compute_binary_checksum(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Guarded single-byte write shared with the 1000 Hz ISR reader. */
static void set_failover_flag(uint8_t value)
{
    EnterCriticalSection(&s_state_lock);
    s_shared_state_buffer.failover_active = value;
    LeaveCriticalSection(&s_state_lock);
}

void layer2_core_init(void)
{
    if (!s_state_lock_ready) {
        InitializeCriticalSection(&s_state_lock);
        s_state_lock_ready = true;
    }

    memset(&s_shared_state_buffer, 0, sizeof(s_shared_state_buffer));
    memset(&s_watchdog_supervisor, 0, sizeof(s_watchdog_supervisor));

    s_shared_state_buffer.magic_header = 0x50524F4D; /* 'PROM' */
    s_shared_state_buffer.active_screen = (uint8_t)SCREEN_BOOT;
    s_shared_state_buffer.dirty_flag = 1;
    s_shared_state_buffer.heartbeat_counter = 0;

    s_watchdog_supervisor.hw_loop_alive = true;
    s_watchdog_supervisor.sw_loop_alive = true;
    s_watchdog_supervisor.system_healthy = true;
    s_watchdog_supervisor.last_hw_ping_ms = layer0_get_system_time_ms();
    s_watchdog_supervisor.last_sw_ping_ms = layer0_get_system_time_ms();

    s_active_screen = SCREEN_BOOT;
    s_previous_screen = SCREEN_BOOT;

    printf("[LAYER 2 CORE] Static Shared State Buffer & FSM Controller initialized.\n");
}

void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv)
{
    uint64_t now = layer0_get_system_time_ms();

    EnterCriticalSection(&s_state_lock);

    s_shared_state_buffer.timestamp_ms = now;
    s_shared_state_buffer.sensor_temp_mC = temp_mC;
    s_shared_state_buffer.sensor_pressure_kPa = press_kPa;
    s_shared_state_buffer.sensor_rpm = rpm;
    s_shared_state_buffer.sensor_bus_mv = bus_mv;
    s_shared_state_buffer.heartbeat_counter++;
    s_shared_state_buffer.dirty_flag = 1;

    /* Check alarm thresholds */
    if (temp_mC > 60000 || press_kPa > 1300 || bus_mv < 22000) {
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_CRITICAL;
    } else if (temp_mC > 52000 || press_kPa > 1200) {
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_WARNING;
    } else {
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_NONE;
    }

    /* NOTE: the CRC is deliberately NOT computed here. This runs 1000x/sec but
     * the checksum is only ever consumed by a 30 Hz reader, so it is calculated
     * once per snapshot in layer2_copy_state_buffer() instead. */

    LeaveCriticalSection(&s_state_lock);
}

const shared_state_buffer_t* layer2_get_state_buffer(void)
{
    return &s_shared_state_buffer;
}

void layer2_copy_state_buffer(shared_state_buffer_t *out)
{
    if (!out) return;
    EnterCriticalSection(&s_state_lock);
    memcpy(out, &s_shared_state_buffer, sizeof(*out));
    LeaveCriticalSection(&s_state_lock);

    /* Seal the snapshot outside the lock: the CRC covers everything but itself. */
    out->checksum = compute_binary_checksum((const uint8_t *)out,
                                            sizeof(*out) - sizeof(out->checksum));
}

bool layer2_consume_dirty_flag(void)
{
    bool was_dirty;
    EnterCriticalSection(&s_state_lock);
    was_dirty = (s_shared_state_buffer.dirty_flag != 0);
    s_shared_state_buffer.dirty_flag = 0;
    LeaveCriticalSection(&s_state_lock);
    return was_dirty;
}

screen_id_t layer2_fsm_get_active_screen(void)
{
    return s_active_screen;
}

bool layer2_fsm_request_screen_change(screen_id_t new_screen)
{
    if (new_screen >= SCREEN_COUNT) {
        printf("[LAYER 2 FSM] Rejected screen change: Out of bounds (%d)\n", new_screen);
        return false;
    }

    if (s_active_screen == new_screen) {
        return true;
    }

    s_previous_screen = s_active_screen;
    s_active_screen = new_screen;

    /* Recursive CRITICAL_SECTION: safe even when called from layer2_fsm_process_event. */
    EnterCriticalSection(&s_state_lock);
    s_shared_state_buffer.active_screen = (uint8_t)new_screen;
    s_shared_state_buffer.dirty_flag = 1;
    LeaveCriticalSection(&s_state_lock);

    printf("[LAYER 2 FSM] Screen Transition: %d -> %d\n", s_previous_screen, s_active_screen);
    return true;
}

void layer2_fsm_process_event(input_key_t key_event)
{
    if (key_event == KEY_NONE) return;

    /* Global key bindings */
    if (key_event == KEY_TOGGLE_FAILOVER) {
        if (s_active_screen == SCREEN_FAILOVER_STANDBY) {
            layer2_fsm_request_screen_change(s_previous_screen);
        } else {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
        }
        set_failover_flag((s_active_screen == SCREEN_FAILOVER_STANDBY) ? 1 : 0);
        return;
    }

    if (key_event == KEY_ALARM_ACK) {
        EnterCriticalSection(&s_state_lock);
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_NONE;
        LeaveCriticalSection(&s_state_lock);
        printf("[LAYER 2 FSM] Alarm Acknowledged by User Key.\n");
        return;
    }

    /* Screen-specific state navigation */
    switch (s_active_screen) {
        case SCREEN_BOOT:
            if (key_event == KEY_SELECT || key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_DASHBOARD:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_SETTINGS);
            } else if (key_event == KEY_SELECT) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            }
            break;

        case SCREEN_DIAGNOSTICS:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_ALARM:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_SETTINGS);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_SETTINGS:
            if (key_event == KEY_NEXT) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            } else if (key_event == KEY_PREV) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            } else if (key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
            }
            break;

        case SCREEN_FAILOVER_STANDBY:
            if (key_event == KEY_SELECT || key_event == KEY_BACK) {
                layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
                set_failover_flag(0);
            }
            break;

        default:
            break;
    }
}

void layer2_watchdog_report_software_alive(void)
{
    s_watchdog_supervisor.last_sw_ping_ms = layer0_get_system_time_ms();
}

void layer2_watchdog_supervisor_tick(void)
{
    uint64_t now = layer0_get_system_time_ms();
    uint64_t last_hw = layer1_get_last_hardware_heartbeat_ms();
    uint64_t last_sw = s_watchdog_supervisor.last_sw_ping_ms;

    s_watchdog_supervisor.last_hw_ping_ms = last_hw;

    /* Evaluate Hardware & Software Heartbeat Age */
    s_watchdog_supervisor.hw_loop_alive = ((now - last_hw) <= WATCHDOG_MAX_AGE_MS);
    s_watchdog_supervisor.sw_loop_alive = ((now - last_sw) <= WATCHDOG_MAX_AGE_MS);

    /* Dual-Loop AND Check: BOTH loops must be alive! */
    s_watchdog_supervisor.system_healthy = 
        (s_watchdog_supervisor.hw_loop_alive && s_watchdog_supervisor.sw_loop_alive);

    if (s_watchdog_supervisor.system_healthy) {
        /* Pet physical hardware watchdog */
        layer0_pet_hardware_watchdog();
        s_watchdog_supervisor.total_watchdog_pets++;
    } else {
        /* Watchdog Fault Detected! */
        s_watchdog_supervisor.watchdog_fault_count++;
        printf("[WATCHDOG SUPERVISOR] WARNING: Dual-loop AND check failed! HW Alive: %d, SW Alive: %d\n",
               s_watchdog_supervisor.hw_loop_alive, s_watchdog_supervisor.sw_loop_alive);
        
        /* Auto-engage Hot Standby Failover FSM mode */
        if (s_active_screen != SCREEN_FAILOVER_STANDBY) {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            set_failover_flag(1);
        }
    }
}

const watchdog_supervisor_t* layer2_get_watchdog_status(void)
{
    return &s_watchdog_supervisor;
}
