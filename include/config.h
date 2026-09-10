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
#define DISPLAY_COLOR_DEPTH    4      /* 4bpp palettized: 2 pixels per byte  */
/*
 * 4bpp packs two pixels into one byte: the EVEN x pixel occupies the high
 * nibble, the ODD x pixel the low nibble (Windows DIB nibble order).
 * DIB scanlines are DWORD-aligned; 800/2 = 400 is already a multiple of 4.
 */
#define DISPLAY_BYTES_PER_ROW  ((DISPLAY_WIDTH + 1) / 2)
#define DISPLAY_STRIDE         ((DISPLAY_BYTES_PER_ROW + 3) & ~3)
#define PALETTE_MAX_SLOTS      16    /* 4bpp hard ceiling; PAL_COUNT must fit */

/*
 * --- Partial Draw Buffer (banded rendering) ---
 * There is NO full-screen framebuffer. The logical surface stays 800x480, but
 * it is rasterized one horizontal band at a time into a single small strip that
 * is blitted immediately. This is the LVGL partial-draw-buffer model, and it
 * cuts pixel memory from 187 KB (full 4bpp frame) to under 19 KB.
 *
 * DISPLAY_HEIGHT must be an exact multiple of DISPLAY_BAND_HEIGHT.
 */
#define DISPLAY_BAND_HEIGHT    48
#define DISPLAY_BAND_COUNT     (DISPLAY_HEIGHT / DISPLAY_BAND_HEIGHT)
#define DISPLAY_BUF_SIZE       (DISPLAY_STRIDE * DISPLAY_BAND_HEIGHT)
#define DISPLAY_FULLFRAME_SIZE (DISPLAY_STRIDE * DISPLAY_HEIGHT) /* for reporting */

/*
 * --- Palette Slots ---
 * The framebuffer stores one byte per pixel: an index into the active palette.
 * Drawing code names a *semantic role*, never a literal colour, so switching
 * theme is a palette rewrite (SetDIBColorTable) rather than a re-render.
 */
typedef enum {
    PAL_BG = 0,          /* Screen background */
    PAL_CARD,            /* Card / panel fill */
    PAL_PRIMARY,         /* Primary accent */
    PAL_SECONDARY,       /* Secondary accent */
    PAL_TEXT,            /* Primary text */
    PAL_TEXT_DIM,        /* Secondary / muted text */
    PAL_ALARM_CRIT,      /* Critical alarm */
    PAL_ALARM_OK,        /* Healthy / nominal */
    PAL_BLACK,           /* Text on a light fill */
    PAL_WHITE,           /* Text on a dark fill */
    PAL_COUNT
} palette_index_t;

/* --- Timing Specifications --- */
#define SENSOR_ISR_FREQ_HZ     1000   /* Layer 1: Sensor & Fieldbus ISR (1000 Hz / 1 ms tick) */
#define UI_TIMER_FREQ_HZ       30     /* Layer 3: LVGL UI Presentation Timer (30 Hz / ~33 ms frame) */
#define UI_FRAME_PERIOD_MS     (1000 / UI_TIMER_FREQ_HZ)

/* --- Watchdog Supervisor Specifications --- */
#define WATCHDOG_MAX_AGE_MS    500    /* Maximum allowed heartbeat silence before failover */
#define WATCHDOG_CHECK_PERIOD_MS 100  /* Supervisor dual-loop validation cycle */
/* The metric poll loop runs at ~2 Hz, so it needs its own (longer) liveness
 * window; judging it against the 500 ms ISR window would flag it dead always. */
#define WATCHDOG_METRICS_MAX_AGE_MS 4000

/* --- Host Telemetry Poll Timing (Layer 1 metric provider engine) --- */
#define METRIC_POLL_PERIOD_MS      500   /* 2 Hz base tick for cheap kernel32 sources */
#define METRIC_SLOW_POLL_PERIOD_MS 5000  /* 0.2 Hz for PDH/WMI-backed sources */
#define METRIC_PROBE_TIMEOUT_MS    1500  /* bounded one-shot probe at startup */

/* --- Resident Set Management --- */
/* Cadence for returning cold pages to the OS. See trim_working_set(). */
#define WORKING_SET_TRIM_PERIOD_MS 3000

/* --- Trend Graph --- */
/* One byte per sample per metric: 6 x 96 = 576 bytes of static history. */
#define METRIC_HISTORY_LEN     96
#define METRIC_HISTORY_PERIOD_MS 2000  /* graph advances at 0.5 Hz, bounded */

/* --- Alarm Engine --- */
#define ALARM_EVENT_LOG_DEPTH  8     /* latched edge-triggered events (ring) */

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
