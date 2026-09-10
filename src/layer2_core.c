/**
 * @file layer2_core.c
 * @brief Layer 2: Core State Management, Binary Serialization, FSM & Watchdog Implementation
 */

#include "../include/layer2_core.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include <stdio.h>
#include <string.h>

/* Static pre-allocated Shared State Buffer (Zero Heap Allocation) */
static shared_state_buffer_t s_shared_state_buffer;
static watchdog_supervisor_t s_watchdog_supervisor;

/* Static FSM state */
static screen_id_t s_active_screen = SCREEN_DASHBOARD;
static screen_id_t s_previous_screen = SCREEN_DASHBOARD;

/* Binary CRC16 / Checksum calculation for data integrity */
static uint16_t compute_binary_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint16_t)(sum + data[i]);
    }
    return sum;
}

static bool s_alarm_acknowledged_by_user = false;
static bool s_prev_alarm_state = false;
static uint64_t s_init_time_ms = 0;
static uint64_t s_last_audio_beep_ms = 0;

void layer2_core_init(void)
{
    memset(&s_shared_state_buffer, 0, sizeof(s_shared_state_buffer));
    memset(&s_watchdog_supervisor, 0, sizeof(s_watchdog_supervisor));

    s_shared_state_buffer.magic_header = 0x50524F4D; /* 'PROM' */
    s_shared_state_buffer.active_screen = (uint8_t)SCREEN_DASHBOARD;
    s_shared_state_buffer.dirty_flag = 1;
    s_shared_state_buffer.heartbeat_counter = 0;

    s_watchdog_supervisor.hw_loop_alive = true;
    s_watchdog_supervisor.sw_loop_alive = true;
    s_watchdog_supervisor.system_healthy = true;
    s_watchdog_supervisor.last_hw_ping_ms = layer0_get_system_time_ms();
    s_watchdog_supervisor.last_sw_ping_ms = layer0_get_system_time_ms();

    s_active_screen = SCREEN_DASHBOARD;
    s_previous_screen = SCREEN_DASHBOARD;
    s_alarm_acknowledged_by_user = false;
    s_prev_alarm_state = false;
    s_init_time_ms = layer0_get_system_time_ms();

    printf("[LAYER 2 CORE] Static Shared State Buffer & FSM Controller initialized to Dashboard Screen.\n");
}

#include <windows.h>
#include <mmsystem.h>

void layer2_update_state_binary(uint32_t temp_mC, uint32_t press_kPa, uint32_t rpm, uint32_t bus_mv)
{
    uint64_t now = layer0_get_system_time_ms();

    s_shared_state_buffer.timestamp_ms = now;
    s_shared_state_buffer.sensor_temp_mC = temp_mC;
    s_shared_state_buffer.sensor_pressure_kPa = press_kPa;
    s_shared_state_buffer.sensor_rpm = rpm;
    s_shared_state_buffer.sensor_bus_mv = bus_mv;
    s_shared_state_buffer.heartbeat_counter++;
    s_shared_state_buffer.dirty_flag = 1;

    bool is_alarm_active = (temp_mC > 45000 || press_kPa > 85 || bus_mv < 22000);
    uint64_t elapsed_since_init = now - s_init_time_ms;

    /* 1. Auto-switch to SCREEN_ALARM & Critical Severity evaluation */
    if (is_alarm_active) {
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_CRITICAL;
        if (elapsed_since_init > 2000 && !s_alarm_acknowledged_by_user) {
            if (s_active_screen != SCREEN_ALARM && s_active_screen != SCREEN_FAILOVER_STANDBY) {
                layer2_fsm_request_screen_change(SCREEN_ALARM);
            }
        }
    } else {
        s_alarm_acknowledged_by_user = false; /* Reset ack flag when temp normalizes */
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_NONE;
    }
    s_prev_alarm_state = is_alarm_active;

    /* 2. Continuous Loud Sound Alarm (Plays every 300 ms through Windows Sound Card until temp < 45.0 C) */
    if (temp_mC > 45000) {
        if ((now - s_last_audio_beep_ms) >= 300) {
            s_last_audio_beep_ms = now;
            MessageBeep(0xFFFFFFFF); /* Standard Windows Master Sound Alert */
            MessageBeep(MB_ICONHAND);
            MessageBeep(MB_OK);
            PlaySoundA("SystemHand", NULL, SND_ALIAS | SND_ASYNC);
            Beep(2000, 100);
            Beep(1200, 100);
        }
    }

    /* Update binary payload checksum */
    size_t payload_len = sizeof(shared_state_buffer_t) - sizeof(uint16_t);
    s_shared_state_buffer.checksum = compute_binary_checksum((const uint8_t*)&s_shared_state_buffer, payload_len);
}

const shared_state_buffer_t* layer2_get_state_buffer(void)
{
    return &s_shared_state_buffer;
}

bool layer2_consume_dirty_flag(void)
{
    if (s_shared_state_buffer.dirty_flag) {
        s_shared_state_buffer.dirty_flag = 0;
        return true;
    }
    return false;
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
    s_shared_state_buffer.active_screen = (uint8_t)new_screen;
    s_shared_state_buffer.dirty_flag = 1;

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
            s_shared_state_buffer.failover_active = 0;
        } else {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
            s_shared_state_buffer.failover_active = 1;
        }
        return;
    }

    if (key_event == KEY_ALARM_ACK) {
        s_alarm_acknowledged_by_user = true;
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_NONE;
        layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
        printf("[LAYER 2 FSM] Alarm Acknowledged. Transitioned to Dashboard Screen.\n");
        return;
    }

    /* Screen-specific state navigation */
    switch (s_active_screen) {

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
                s_shared_state_buffer.failover_active = 0;
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
            s_shared_state_buffer.failover_active = 1;
        }
    }
}

const watchdog_supervisor_t* layer2_get_watchdog_status(void)
{
    return &s_watchdog_supervisor;
}
