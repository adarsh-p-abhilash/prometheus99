/**
 * @file layer2_core.c
 * @brief Layer 2: Shared State, FSM, Hysteresis Alarm Engine & Watchdog
 */

#include "../include/layer2_core.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ===========================================================================
 * ALARM CONFIGURATION -- data, not logic.
 *
 * Every threshold below is a hysteresis BAND plus a debounce depth. Nothing in
 * the engine hardcodes a number; changing alarm behaviour means editing this
 * table only. Units follow metric_id_t: tenths of a percent for the load
 * metrics, deci-Celsius for temperature, RPM for the fan.
 *
 *                          arm WARN  clear WARN  arm CRIT  clear CRIT  confirm
 * CPU LOAD                    85.0%      75.0%     95.0%      90.0%       3
 * MEMORY                      85.0%      78.0%     94.0%      90.0%       3
 * GPU LOAD                    90.0%      80.0%     97.0%      93.0%       2
 * BATTERY (inverted)          25.0%      35.0%     10.0%      18.0%       2
 * CPU TEMP                    85.0 C     78.0 C    95.0 C     90.0 C      3
 * FAN                         4000 rpm   3400 rpm  5500 rpm   5000 rpm    3
 *
 * A CPU running at 84-86% therefore CANNOT toggle the alarm: it must reach
 * 85.0% for 3 consecutive polls to arm, and fall below 75.0% for 3 consecutive
 * polls to clear.
 * ==========================================================================*/
const alarm_rule_t k_alarm_rules[METRIC_COUNT] = {
    [METRIC_CPU_LOAD] = { METRIC_CPU_LOAD, "CPU LOAD HIGH",   false,
                          850,  750,  950,  900, 3, METRIC_COUNT },
    [METRIC_MEM_USED] = { METRIC_MEM_USED, "MEMORY PRESSURE", false,
                          850,  780,  940,  900, 3, METRIC_COUNT },
    [METRIC_GPU_LOAD] = { METRIC_GPU_LOAD, "GPU SATURATED",   false,
                          900,  800,  970,  930, 2, METRIC_COUNT },
    [METRIC_BATTERY]  = { METRIC_BATTERY,  "BATTERY LOW",     true,
                          250,  350,  100,  180, 2, METRIC_COUNT },
    /* Thermal and fan are downstream symptoms of sustained CPU load, so they
     * declare a root cause and are logged as dependent rather than firing an
     * independent alarm tile when the root is already active. */
    [METRIC_CPU_TEMP] = { METRIC_CPU_TEMP, "CPU OVER-TEMP",   false,
                          850,  780,  950,  900, 3, METRIC_CPU_LOAD },
    [METRIC_FAN_RPM]  = { METRIC_FAN_RPM,  "FAN OVERSPEED",   false,
                          4000, 3400, 5500, 5000, 3, METRIC_CPU_TEMP }
};

/* Static pre-allocated Shared State Buffer (Zero Heap Allocation) */
static shared_state_buffer_t s_shared_state_buffer;
static shared_state_buffer_t s_last_published;   /* for change detection */
static watchdog_supervisor_t s_watchdog_supervisor;

/* Per-metric alarm FSM state. Never touched until the metric has produced a
 * real sample, which is what keeps cold start from emitting a false alarm. */
typedef struct {
    alarm_level_t level;          /* committed level                     */
    alarm_level_t pending;        /* candidate awaiting confirmation     */
    uint8_t       pending_count;  /* consecutive agreeing samples        */
    bool          seeded;         /* first real sample has arrived       */
    bool          dependent;      /* suppressed as a downstream symptom  */
} alarm_fsm_t;

static alarm_fsm_t s_alarm_fsm[METRIC_COUNT];

/* Trend history: one ring per metric, advanced on a fixed slow cadence. */
static uint16_t s_history[METRIC_COUNT][METRIC_HISTORY_LEN];
static int      s_history_head;
static int      s_history_fill;
static uint64_t s_history_next_ms;

/* Latched edge-triggered event ring. */
static alarm_event_t s_event_log[ALARM_EVENT_LOG_DEPTH];
static uint32_t      s_event_total;
static int           s_event_head;

/*
 * The Shared State Buffer is written by the metric poll thread and read by the
 * 30 Hz presentation thread. This lock keeps each reader/writer from observing
 * a half-updated packet (torn 64-bit timestamp, mismatched fields).
 */
static CRITICAL_SECTION s_state_lock;
static bool s_state_lock_ready = false;

/* Static FSM state */
static screen_id_t s_active_screen = SCREEN_BOOT;
static screen_id_t s_previous_screen = SCREEN_BOOT;

/*
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the packed payload.
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

/* Guarded single-byte write shared with the metric poll thread. */
static void set_failover_flag(uint8_t value)
{
    EnterCriticalSection(&s_state_lock);
    s_shared_state_buffer.failover_active = value;
    LeaveCriticalSection(&s_state_lock);
}

static void mark_dirty_locked(void)
{
    s_shared_state_buffer.dirty_flag = 1;
    s_shared_state_buffer.ui_version++;
}

void layer2_core_init(void)
{
    if (!s_state_lock_ready) {
        InitializeCriticalSection(&s_state_lock);
        s_state_lock_ready = true;
    }

    memset(&s_shared_state_buffer, 0, sizeof(s_shared_state_buffer));
    memset(&s_last_published, 0, sizeof(s_last_published));
    memset(&s_watchdog_supervisor, 0, sizeof(s_watchdog_supervisor));
    memset(s_alarm_fsm, 0, sizeof(s_alarm_fsm));
    memset(s_event_log, 0, sizeof(s_event_log));
    s_event_total = 0;
    s_event_head = 0;
    memset(s_history, 0, sizeof(s_history));
    s_history_head = 0;
    s_history_fill = 0;
    s_history_next_ms = 0;

    s_shared_state_buffer.magic_header = 0x50524F4D; /* 'PROM' */
    s_shared_state_buffer.active_screen = (uint8_t)SCREEN_BOOT;
    s_shared_state_buffer.dirty_flag = 1;

    for (int i = 0; i < METRIC_COUNT; i++) {
        s_alarm_fsm[i].level   = ALARM_NONE;
        s_alarm_fsm[i].pending = ALARM_NONE;
        s_alarm_fsm[i].seeded  = false;
        s_shared_state_buffer.metric_state[i] = (uint8_t)MSTATE_UNAVAILABLE;
        s_shared_state_buffer.metric_alarm[i] = (uint8_t)ALARM_NONE;
    }

    uint64_t now = layer0_get_system_time_ms();
    s_watchdog_supervisor.hw_loop_alive = true;
    s_watchdog_supervisor.sw_loop_alive = true;
    s_watchdog_supervisor.metrics_loop_alive = true;
    s_watchdog_supervisor.system_healthy = true;
    s_watchdog_supervisor.last_hw_ping_ms = now;
    s_watchdog_supervisor.last_sw_ping_ms = now;
    s_watchdog_supervisor.last_metrics_ping_ms = now;

    s_active_screen = SCREEN_BOOT;
    s_previous_screen = SCREEN_BOOT;

    printf("[LAYER 2 CORE] Shared State, FSM & hysteresis alarm engine initialized.\n");
}

void layer2_tick_isr_counter(void)
{
    /*
     * 1000 Hz liveness only. Deliberately NOT dirty-marking and deliberately
     * not locking: a torn read of a monotonically increasing counter is
     * harmless, and taking a critical section a thousand times a second to
     * publish a number nothing renders live would be pure overhead.
     */
    s_shared_state_buffer.isr_tick_count++;
}

/* --- Hysteresis evaluation -------------------------------------------------
 * The candidate level depends on the CURRENT level: that asymmetry is what
 * makes the band a band. A value inside the dead zone returns the level it is
 * already in, so it cannot toggle.
 */
static alarm_level_t evaluate_level(const alarm_rule_t *r, int32_t v, alarm_level_t cur)
{
    if (r->invert) {                      /* lower is worse (battery) */
        if (cur == ALARM_CRITICAL) return (v > r->crit_off) ? ALARM_WARNING : ALARM_CRITICAL;
        if (cur == ALARM_WARNING) {
            if (v <= r->crit_on)  return ALARM_CRITICAL;
            return (v > r->warn_off) ? ALARM_NONE : ALARM_WARNING;
        }
        if (v <= r->crit_on) return ALARM_CRITICAL;
        if (v <= r->warn_on) return ALARM_WARNING;
        return ALARM_NONE;
    }
    /* higher is worse */
    if (cur == ALARM_CRITICAL) return (v < r->crit_off) ? ALARM_WARNING : ALARM_CRITICAL;
    if (cur == ALARM_WARNING) {
        if (v >= r->crit_on)  return ALARM_CRITICAL;
        return (v < r->warn_off) ? ALARM_NONE : ALARM_WARNING;
    }
    if (v >= r->crit_on) return ALARM_CRITICAL;
    if (v >= r->warn_on) return ALARM_WARNING;
    return ALARM_NONE;
}

static void log_alarm_event(uint64_t t, metric_id_t m, alarm_level_t from,
                            alarm_level_t to, bool dependent, int32_t value)
{
    alarm_event_t *e = &s_event_log[s_event_head];
    e->t_ms       = t;
    e->metric     = (uint8_t)m;
    e->from_level = (uint8_t)from;
    e->to_level   = (uint8_t)to;
    e->dependent  = dependent ? 1u : 0u;
    e->value      = value;
    s_event_head  = (s_event_head + 1) % ALARM_EVENT_LOG_DEPTH;
    s_event_total++;

    if (dependent) {
        printf("[ALARM] %-16s %d -> %d (value %ld) -- related to %s, suppressed\n",
               k_alarm_rules[m].label, (int)from, (int)to, (long)value,
               k_alarm_rules[k_alarm_rules[m].root_cause].label);
    } else {
        printf("[ALARM] %-16s %d -> %d (value %ld)\n",
               k_alarm_rules[m].label, (int)from, (int)to, (long)value);
    }
}

void layer2_publish_metrics(const metric_sample_t samples[METRIC_COUNT])
{
    if (!samples) return;
    uint64_t now = layer0_get_system_time_ms();

    /* --- Pass 1: run each metric's debounced hysteresis FSM ---------------- */
    for (int i = 0; i < METRIC_COUNT; i++) {
        const metric_sample_t *s = &samples[i];
        alarm_fsm_t *f = &s_alarm_fsm[i];

        if (s->state != MSTATE_OK) {
            /* No trustworthy data: hold at NONE, do not evaluate, do not fire. */
            f->level = ALARM_NONE;
            f->pending = ALARM_NONE;
            f->pending_count = 0;
            continue;
        }

        if (!f->seeded) {
            /*
             * Cold start: the debounce window is seeded from this FIRST real
             * sample, never from the zero-initialised struct. The committed
             * level stays NONE, so even a genuinely out-of-band first reading
             * must still survive confirm_samples polls before it can fire.
             */
            f->seeded = true;
            f->level = ALARM_NONE;
            f->pending = ALARM_NONE;
            f->pending_count = 0;
        }

        const alarm_rule_t *r = &k_alarm_rules[i];
        alarm_level_t candidate = evaluate_level(r, s->quantized, f->level);

        if (candidate == f->level) {
            f->pending = f->level;      /* back inside the band: reset debounce */
            f->pending_count = 0;
            continue;
        }
        if (candidate != f->pending) {
            f->pending = candidate;     /* new candidate: restart the count     */
            f->pending_count = 1;
            continue;
        }
        if (f->pending_count < 255) f->pending_count++;

        if (f->pending_count < r->confirm_samples) {
            continue;                   /* not confirmed yet: single-sample
                                         * spikes never reach the FSM          */
        }

        /* --- Confirmed transition: fire exactly ONE edge-triggered event --- */
        alarm_level_t from = f->level;
        f->level = candidate;
        f->pending_count = 0;

        /* Correlated-symptom suppression: if this metric declares a root cause
         * and that root is already in alarm, log the transition as dependent
         * instead of raising an independent alarm. */
        bool dependent = false;
        if (r->root_cause < METRIC_COUNT && candidate != ALARM_NONE) {
            alarm_level_t root = s_alarm_fsm[r->root_cause].level;
            dependent = (root >= ALARM_WARNING);
        }
        f->dependent = dependent;

        log_alarm_event(now, (metric_id_t)i, from, candidate, dependent, s->quantized);
    }

    /* --- Pass 2: stage the new buffer and diff it against the last one ----- */
    EnterCriticalSection(&s_state_lock);

    s_shared_state_buffer.timestamp_ms = now;
    s_shared_state_buffer.publish_counter++;

    uint8_t worst = (uint8_t)ALARM_NONE;
    for (int i = 0; i < METRIC_COUNT; i++) {
        s_shared_state_buffer.metric_value[i]     = samples[i].quantized;
        s_shared_state_buffer.metric_aux[i]       = samples[i].aux;
        s_shared_state_buffer.metric_state[i]     = (uint8_t)samples[i].state;
        s_shared_state_buffer.metric_alarm[i]     = (uint8_t)s_alarm_fsm[i].level;
        s_shared_state_buffer.metric_dependent[i] = s_alarm_fsm[i].dependent ? 1u : 0u;

        /* A dependent symptom does not raise overall severity: the root alarm
         * already represents the fault. */
        if (!s_alarm_fsm[i].dependent && s_alarm_fsm[i].level > worst) {
            worst = (uint8_t)s_alarm_fsm[i].level;
        }
    }
    s_shared_state_buffer.alarm_severity = worst;
    s_shared_state_buffer.alarm_event_count = s_event_total;

    /*
     * Dirty policy. Compare ONLY what the operator can actually see: quantized
     * values, availability state, alarm level and dependency. timestamp_ms and
     * publish_counter are excluded by construction -- they change every poll and
     * would re-dirty the UI forever, which is exactly the bug being fixed.
     */
    bool changed = false;
    for (int i = 0; i < METRIC_COUNT && !changed; i++) {
        if (s_shared_state_buffer.metric_value[i]     != s_last_published.metric_value[i]  ||
            s_shared_state_buffer.metric_aux[i]       != s_last_published.metric_aux[i]    ||
            s_shared_state_buffer.metric_state[i]     != s_last_published.metric_state[i]  ||
            s_shared_state_buffer.metric_alarm[i]     != s_last_published.metric_alarm[i]  ||
            s_shared_state_buffer.metric_dependent[i] != s_last_published.metric_dependent[i]) {
            changed = true;
        }
    }
    if (s_shared_state_buffer.alarm_severity != s_last_published.alarm_severity) changed = true;

    /*
     * Trend history advances on its own slow clock, independent of the poll
     * rate. This is the ONLY periodic dirty source in the runtime and it is
     * hard-bounded to 1/METRIC_HISTORY_PERIOD_MS, so the graph can never
     * reintroduce per-sample redraw spam.
     */
    if (s_history_next_ms == 0) s_history_next_ms = now + METRIC_HISTORY_PERIOD_MS;
    if (now >= s_history_next_ms) {
        s_history_next_ms = now + METRIC_HISTORY_PERIOD_MS;
        for (int i = 0; i < METRIC_COUNT; i++) {
            int32_t v = (samples[i].state == MSTATE_OK) ? samples[i].quantized : 0;
            if (v < 0) v = 0;
            if (v > 0xFFFF) v = 0xFFFF;
            s_history[i][s_history_head] = (uint16_t)v;
        }
        s_history_head = (s_history_head + 1) % METRIC_HISTORY_LEN;
        if (s_history_fill < METRIC_HISTORY_LEN) s_history_fill++;
        changed = true;
    }

    if (changed) {
        mark_dirty_locked();
        memcpy(s_last_published.metric_value,     s_shared_state_buffer.metric_value,     sizeof(s_last_published.metric_value));
        memcpy(s_last_published.metric_aux,       s_shared_state_buffer.metric_aux,       sizeof(s_last_published.metric_aux));
        memcpy(s_last_published.metric_state,     s_shared_state_buffer.metric_state,     sizeof(s_last_published.metric_state));
        memcpy(s_last_published.metric_alarm,     s_shared_state_buffer.metric_alarm,     sizeof(s_last_published.metric_alarm));
        memcpy(s_last_published.metric_dependent, s_shared_state_buffer.metric_dependent, sizeof(s_last_published.metric_dependent));
        s_last_published.alarm_severity = s_shared_state_buffer.alarm_severity;
    }

    LeaveCriticalSection(&s_state_lock);
}

int layer2_get_metric_history(metric_id_t id, uint16_t out[METRIC_HISTORY_LEN])
{
    if (!out || id >= METRIC_COUNT) return 0;
    EnterCriticalSection(&s_state_lock);
    int n = s_history_fill;
    for (int i = 0; i < n; i++) {
        int idx = (s_history_head - n + i + METRIC_HISTORY_LEN * 2) % METRIC_HISTORY_LEN;
        out[i] = s_history[id][idx];
    }
    LeaveCriticalSection(&s_state_lock);
    return n;
}

int layer2_get_alarm_events(alarm_event_t out[ALARM_EVENT_LOG_DEPTH])
{
    if (!out) return 0;
    int n = (s_event_total < ALARM_EVENT_LOG_DEPTH)
          ? (int)s_event_total : ALARM_EVENT_LOG_DEPTH;
    EnterCriticalSection(&s_state_lock);
    for (int i = 0; i < n; i++) {
        int idx = (s_event_head - n + i + ALARM_EVENT_LOG_DEPTH * 2) % ALARM_EVENT_LOG_DEPTH;
        out[i] = s_event_log[idx];
    }
    LeaveCriticalSection(&s_state_lock);
    return n;
}

uint32_t layer2_get_alarm_event_count(void)
{
    return s_event_total;
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
    mark_dirty_locked();
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
        /*
         * Acknowledge clears the LATCHED presentation, not the FSM level: if the
         * underlying condition is still true the alarm remains armed and will
         * not re-fire an event, because transitions are edge-triggered.
         */
        EnterCriticalSection(&s_state_lock);
        s_shared_state_buffer.alarm_severity = (uint8_t)ALARM_NONE;
        s_last_published.alarm_severity = (uint8_t)ALARM_NONE;
        mark_dirty_locked();
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
    uint64_t last_mx = layer1_metrics_last_heartbeat_ms();

    s_watchdog_supervisor.last_hw_ping_ms = last_hw;
    s_watchdog_supervisor.last_metrics_ping_ms = last_mx;

    /* Evaluate every loop against its own cadence. The metric poll runs at
     * ~2 Hz, so judging it against the 500 ms ISR window would flag it dead. */
    s_watchdog_supervisor.hw_loop_alive = ((now - last_hw) <= WATCHDOG_MAX_AGE_MS);
    s_watchdog_supervisor.sw_loop_alive = ((now - last_sw) <= WATCHDOG_MAX_AGE_MS);
    s_watchdog_supervisor.metrics_loop_alive =
        ((now - last_mx) <= WATCHDOG_METRICS_MAX_AGE_MS);

    /* Triple-Loop AND Check: a wedged provider is now visible here, where a
     * hung PDH or WMI call used to be completely invisible. */
    s_watchdog_supervisor.system_healthy =
        (s_watchdog_supervisor.hw_loop_alive &&
         s_watchdog_supervisor.sw_loop_alive &&
         s_watchdog_supervisor.metrics_loop_alive);

    if (s_watchdog_supervisor.system_healthy) {
        layer0_pet_hardware_watchdog();
        s_watchdog_supervisor.total_watchdog_pets++;
    } else {
        s_watchdog_supervisor.watchdog_fault_count++;
        printf("[WATCHDOG] AND check failed! HW=%d SW=%d METRICS=%d\n",
               s_watchdog_supervisor.hw_loop_alive,
               s_watchdog_supervisor.sw_loop_alive,
               s_watchdog_supervisor.metrics_loop_alive);

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
