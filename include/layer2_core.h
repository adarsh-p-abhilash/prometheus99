/**
 * @file layer2_core.h
 * @brief Layer 2: Core State, Serialization, FSM, Alarm Engine & Watchdog
 *
 * Owns the static binary Shared State Buffer, the deterministic screen FSM,
 * the hysteresis/debounce alarm engine, and the Dual-Loop AND Check Watchdog.
 */

#ifndef LAYER2_CORE_H
#define LAYER2_CORE_H

#include "config.h"
#include "layer1_metrics.h"

/* --- Binary Packed Shared State Buffer Structure --- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic_header;                  /* 0x50524F4D ('PROM')            */
    uint64_t timestamp_ms;                  /* High-resolution timestamp      */

    /* Host telemetry, quantized to display precision (see layer1_metrics.h). */
    int32_t  metric_value[METRIC_COUNT];
    int32_t  metric_aux[METRIC_COUNT];
    uint8_t  metric_state[METRIC_COUNT];    /* metric_state_t                 */
    uint8_t  metric_alarm[METRIC_COUNT];    /* alarm_level_t, post-debounce   */
    uint8_t  metric_dependent[METRIC_COUNT];/* 1 = symptom of a root alarm    */

    uint8_t  alarm_severity;                /* worst independent alarm        */
    uint8_t  active_screen;                 /* screen_id_t                    */
    uint8_t  dirty_flag;                    /* 1 = UI must re-rasterize       */
    uint8_t  failover_active;               /* 1 = Hot Standby engaged        */

    uint32_t isr_tick_count;                /* 1000 Hz loop, liveness proof   */
    uint32_t publish_counter;               /* metric publishes accepted      */
    uint32_t ui_version;                    /* bumps ONLY on visible change   */
    uint32_t alarm_event_count;             /* edge-triggered events, total   */
    uint16_t checksum;                      /* CRC-16/CCITT over payload      */
} shared_state_buffer_t;
#pragma pack(pop)

/* --- Alarm rule: hysteresis band + debounce, as data ----------------------
 * Every threshold is a BAND, not a point. `on` arms the level, `off` clears it,
 * and the gap between them is the hysteresis that stops a value hovering at the
 * boundary from toggling the alarm. `confirm_samples` is the debounce: a breach
 * must persist that many consecutive polls before the FSM moves.
 *
 * For `invert` metrics (battery: low is bad) the comparisons flip, so `on` is
 * below `off`.
 */
typedef struct {
    metric_id_t metric;
    const char *label;
    bool        invert;          /* true  = lower value is worse      */
    int32_t     warn_on;         /* arm WARNING                       */
    int32_t     warn_off;        /* clear WARNING (hysteresis gap)    */
    int32_t     crit_on;         /* arm CRITICAL                      */
    int32_t     crit_off;        /* clear CRITICAL back to WARNING    */
    uint8_t     confirm_samples; /* debounce depth                    */
    metric_id_t root_cause;      /* METRIC_COUNT = independent alarm  */
} alarm_rule_t;

/* Exposed so the diagnostics screen can render the live configuration and
 * prove the thresholds are data, not magic numbers buried in logic. */
extern const alarm_rule_t k_alarm_rules[METRIC_COUNT];

/* --- Latched, edge-triggered alarm event ---------------------------------- */
typedef struct {
    uint64_t t_ms;
    uint8_t  metric;       /* metric_id_t   */
    uint8_t  from_level;   /* alarm_level_t */
    uint8_t  to_level;     /* alarm_level_t */
    uint8_t  dependent;    /* 1 = logged as related to a root alarm */
    int32_t  value;        /* quantized value at the transition     */
} alarm_event_t;

/* --- Watchdog Supervisor Status --- */
typedef struct {
    bool hw_loop_alive;
    bool sw_loop_alive;
    bool metrics_loop_alive;
    uint64_t last_hw_ping_ms;
    uint64_t last_sw_ping_ms;
    uint64_t last_metrics_ping_ms;
    uint32_t total_watchdog_pets;
    uint32_t watchdog_fault_count;
    bool system_healthy;
} watchdog_supervisor_t;

/* --- Core Function Declarations --- */
void layer2_core_init(void);

/* Shared State Management & Binary Serialization */
const shared_state_buffer_t* layer2_get_state_buffer(void);
void layer2_copy_state_buffer(shared_state_buffer_t *out);
bool layer2_consume_dirty_flag(void);

/* 1000 Hz liveness tick. Deliberately does NOT mark the UI dirty. */
void layer2_tick_isr_counter(void);

/* Metric ingest: runs hysteresis + debounce, emits edge events, and sets the
 * dirty flag only when a quantized display value or alarm level actually moved. */
void layer2_publish_metrics(const metric_sample_t samples[METRIC_COUNT]);

/* Trend history for the dashboard graph. Advances on a fixed slow cadence
 * (METRIC_HISTORY_PERIOD_MS), never per poll, so the graph cannot become a new
 * source of redraw spam. Fills out[] oldest-first, returns the sample count. */
int layer2_get_metric_history(metric_id_t id, uint16_t out[METRIC_HISTORY_LEN]);

/* Alarm event log (ring buffer, newest last). Returns how many are valid. */
int  layer2_get_alarm_events(alarm_event_t out[ALARM_EVENT_LOG_DEPTH]);
uint32_t layer2_get_alarm_event_count(void);

/* FSM Controller */
screen_id_t layer2_fsm_get_active_screen(void);
bool layer2_fsm_request_screen_change(screen_id_t new_screen);
void layer2_fsm_process_event(input_key_t key_event);

/* Watchdog Supervisor (Triple-Loop AND Check: ISR, UI, metric poll) */
void layer2_watchdog_report_software_alive(void);
void layer2_watchdog_supervisor_tick(void);
const watchdog_supervisor_t* layer2_get_watchdog_status(void);

#endif /* LAYER2_CORE_H */
