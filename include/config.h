/**
 * @file config.h
 * @brief Prometheus99 HMI Runtime System Configuration
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 * 
 * Target: Pure C99 Standard
 * Graphics Engine: LVGL Presentation Layer
 * Architecture: 4-Layer Static Memory HMI Engine
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --- Display Specifications --- */
#define DISPLAY_WIDTH          800
#define DISPLAY_HEIGHT         480
#define DISPLAY_COLOR_DEPTH    32     /* ARGB8888 / RGB888 32-bit color format */
#define DISPLAY_BUF_SIZE       (DISPLAY_WIDTH * DISPLAY_HEIGHT)

/* --- Timing Specifications --- */
#define SENSOR_ISR_FREQ_HZ     1000   /* Layer 1: Sensor & Fieldbus ISR (1000 Hz / 1 ms tick) */
#define UI_TIMER_FREQ_HZ       30     /* Layer 3: LVGL UI Presentation Timer (30 Hz / ~33 ms frame) */
#define UI_FRAME_PERIOD_MS     (1000 / UI_TIMER_FREQ_HZ)

/* --- Watchdog Supervisor Specifications --- */
#define WATCHDOG_MAX_AGE_MS    500    /* Maximum allowed heartbeat silence before failover */
#define WATCHDOG_CHECK_PERIOD_MS 100  /* Supervisor dual-loop validation cycle */

/* --- Pre-Allocated Screen Identifiers --- */
typedef enum {
    SCREEN_BOOT = 0,
    SCREEN_DASHBOARD,
    SCREEN_DIAGNOSTICS,
    SCREEN_ALARM,
    SCREEN_SETTINGS,
    SCREEN_FAILOVER_STANDBY,
    SCREEN_COUNT
} screen_id_t;

/* --- Unified Input Action Keys --- */
typedef enum {
    KEY_NONE = 0,
    KEY_PREV,
    KEY_NEXT,
    KEY_SELECT,
    KEY_BACK,
    KEY_ALARM_ACK,
    KEY_TOGGLE_FAILOVER,
    KEY_TOGGLE_CONTRAST
} input_key_t;

/* --- Alarm Severity Levels --- */
typedef enum {
    ALARM_NONE = 0,
    ALARM_INFO,
    ALARM_WARNING,
    ALARM_CRITICAL,
    ALARM_EMERGENCY
} alarm_level_t;

#endif /* CONFIG_H */
