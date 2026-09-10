/**
 * @file layer1_hal.c
 * @brief Layer 1: Hardware Abstraction Layer, Drivers & Host Metric Providers
 */

#include "../include/layer1_hal.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_metrics.h"
#include "../include/layer2_core.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>   /* PDH_MORE_DATA */
#include <process.h>

#include "layer1_metrics_providers.inc"

static uint64_t s_last_hw_heartbeat_ms = 0;

/* Statically sized lock-free-by-construction event queues (single producer and
 * single consumer, both on the UI thread). Power-of-two size for cheap wrap. */
#define INPUT_QUEUE_SIZE 16
#define INPUT_QUEUE_MASK (INPUT_QUEUE_SIZE - 1)

static input_key_t s_key_queue[INPUT_QUEUE_SIZE];
static unsigned s_key_head, s_key_tail;

static raw_touch_data_t s_touch_queue[INPUT_QUEUE_SIZE];
static unsigned s_touch_head, s_touch_tail;

void layer1_hal_init(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
    s_key_head = s_key_tail = 0;
    s_touch_head = s_touch_tail = 0;
    printf("[LAYER 1 HAL] Drivers & 1000Hz Sensor ISR initialized.\n");
}

/*
 * Executed by the High-Resolution Interrupt / Thread at 1000 Hz.
 *
 * This loop NO LONGER produces display telemetry. Host vitals come from the
 * metric provider engine on its own low-rate thread. Making a 1000 Hz loop the
 * source of display data was the root cause of the redraw and alarm spam: it
 * marked the UI dirty a thousand times a second.
 *
 * What remains here is exactly what belongs at 1000 Hz: the tick counter and
 * the hardware-side liveness beacon for the dual-loop watchdog.
 */
void layer1_sensor_isr_handler_1000hz(void)
{
    layer2_tick_isr_counter();

    /*
     * Heartbeat for the "hardware" side of the dual-loop watchdog. This must be
     * driven by the 1000 Hz ISR thread itself so the HW loop and the 30 Hz SW
     * loop are genuinely independent liveness signals.
     */
    layer1_report_hardware_alive();
}

void layer1_report_hardware_alive(void)
{
    s_last_hw_heartbeat_ms = layer0_get_system_time_ms();
}

uint64_t layer1_get_last_hardware_heartbeat_ms(void)
{
    return s_last_hw_heartbeat_ms;
}

static display_flush_fn s_display_flush = NULL;

void layer1_register_display_flush(display_flush_fn fn)
{
    s_display_flush = fn;
}

void layer1_display_flush_cb(const display_area_t *area, const uint8_t *color_p)
{
    /*
     * Push one completed band to the panel. The hardware-liveness heartbeat is
     * NOT reported here: this runs on the 30 Hz presentation thread, so using
     * it as the HW heartbeat would make two watchdog loops depend on one
     * thread. The real HW beat comes from layer1_sensor_isr_handler_1000hz().
     */
    if (s_display_flush) s_display_flush(area, color_p);
}

void layer1_queue_button_event(input_key_t key)
{
    if (key == KEY_NONE) return;
    unsigned next = (s_key_head + 1u) & INPUT_QUEUE_MASK;
    if (next == s_key_tail) return; /* full: drop oldest-wins, never block */
    s_key_queue[s_key_head] = key;
    s_key_head = next;
}

bool layer1_poll_button_event(input_key_t *key_out)
{
    if (s_key_tail == s_key_head) return false;
    if (key_out) *key_out = s_key_queue[s_key_tail];
    s_key_tail = (s_key_tail + 1u) & INPUT_QUEUE_MASK;
    return true;
}

void layer1_queue_touch_event(int16_t x, int16_t y, bool pressed)
{
    unsigned next = (s_touch_head + 1u) & INPUT_QUEUE_MASK;
    if (next == s_touch_tail) return;
    s_touch_queue[s_touch_head].x = x;
    s_touch_queue[s_touch_head].y = y;
    s_touch_queue[s_touch_head].is_pressed = pressed;
    s_touch_head = next;
}

bool layer1_poll_touch_event(int16_t *x, int16_t *y, bool *pressed)
{
    if (s_touch_tail == s_touch_head) return false;
    const raw_touch_data_t *t = &s_touch_queue[s_touch_tail];
    if (x) *x = t->x;
    if (y) *y = t->y;
    if (pressed) *pressed = t->is_pressed;
    s_touch_tail = (s_touch_tail + 1u) & INPUT_QUEUE_MASK;
    return true;
}

/* ===========================================================================
 * Metric provider engine
 *
 * Table-driven: adding a host vital is one row here plus one row in the alarm
 * rule table in layer2_core.c. Nothing else changes.
 * ==========================================================================*/

static const metric_provider_t k_providers[METRIC_COUNT] = {
    [METRIC_CPU_LOAD] = { "GetSystemTimes",        METRIC_CPU_LOAD, METRIC_POLL_PERIOD_MS,
                          cpu_probe,   cpu_read,        NULL },
    [METRIC_MEM_USED] = { "GlobalMemoryStatusEx",  METRIC_MEM_USED, METRIC_POLL_PERIOD_MS,
                          mem_probe,   mem_read,        NULL },
    [METRIC_GPU_LOAD] = { "PDH GPU Engine",        METRIC_GPU_LOAD, METRIC_SLOW_POLL_PERIOD_MS,
                          gpu_probe,   gpu_read,        gpu_release },
    [METRIC_BATTERY]  = { "GetSystemPowerStatus",  METRIC_BATTERY,  METRIC_SLOW_POLL_PERIOD_MS,
                          batt_probe,  batt_read,       NULL },
    [METRIC_CPU_TEMP] = { "hwmon shim (optional)", METRIC_CPU_TEMP, METRIC_SLOW_POLL_PERIOD_MS,
                          hwmon_probe, hwmon_read_temp, hwmon_release },
    [METRIC_FAN_RPM]  = { "hwmon shim (optional)", METRIC_FAN_RPM,  METRIC_SLOW_POLL_PERIOD_MS,
                          hwmon_probe, hwmon_read_fan,  NULL }
};

/* Display precision per metric. Raw values are rounded to this step BEFORE any
 * change comparison, so sub-display-resolution noise can never mark UI dirty. */
static const int32_t k_metric_quantum[METRIC_COUNT] = {
    [METRIC_CPU_LOAD] = 10,   /* tenths of % -> whole percent */
    [METRIC_MEM_USED] = 10,   /* whole percent                */
    [METRIC_GPU_LOAD] = 10,   /* whole percent                */
    [METRIC_BATTERY]  = 10,   /* whole percent                */
    [METRIC_CPU_TEMP] = 10,   /* deci-C -> whole degree       */
    [METRIC_FAN_RPM]  = 50    /* nearest 50 RPM               */
};

static const char *k_metric_label[METRIC_COUNT] = {
    "CPU LOAD", "MEMORY", "GPU LOAD", "BATTERY", "CPU TEMP", "FAN"
};
static const char *k_metric_unit[METRIC_COUNT] = { "%", "%", "%", "%", "C", "RPM" };

static metric_sample_t  s_metrics[METRIC_COUNT];
static uint64_t         s_metric_next_due[METRIC_COUNT];
static bool             s_provider_ok[METRIC_COUNT];
static CRITICAL_SECTION s_metric_lock;
static bool             s_metric_lock_ready = false;
static uint64_t         s_metric_heartbeat_ms;

static int32_t quantize(metric_id_t id, int32_t raw)
{
    int32_t q = k_metric_quantum[id];
    if (q <= 1) return raw;
    return ((raw + q / 2) / q) * q;
}

/* --- Bounded one-shot probe ----------------------------------------------
 * Providers backed by a dynamically loaded DLL (PDH today, a WMI/COM bridge
 * tomorrow) can block for an unbounded time on a wedged service. Run those on
 * a throwaway thread and give up after METRIC_PROBE_TIMEOUT_MS.
 *
 * On timeout the thread is abandoned rather than TerminateThread-ed: killing a
 * thread mid-LoadLibrary leaves the loader lock held and deadlocks the process.
 * The orphan finishes on its own and its result is discarded.
 */
typedef struct { bool (*fn)(void); volatile LONG done; volatile LONG ok; } probe_ctx_t;

static unsigned __stdcall probe_thread_proc(void *arg)
{
    probe_ctx_t *ctx = (probe_ctx_t *)arg;
    LONG ok = ctx->fn() ? 1 : 0;
    InterlockedExchange(&ctx->ok, ok);
    InterlockedExchange(&ctx->done, 1);
    return 0;
}

/* Bounded static storage: at most METRIC_COUNT slots ever exist, so an
 * abandoned probe thread always has a valid context to finish writing into. */
static probe_ctx_t s_probe_ctx[METRIC_COUNT];

static bool probe_bounded(metric_id_t id, bool (*fn)(void), bool needs_timeout)
{
    if (!fn) return false;
    if (!needs_timeout) return fn();

    probe_ctx_t *ctx = &s_probe_ctx[id];
    ctx->fn = fn; ctx->done = 0; ctx->ok = 0;

    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, probe_thread_proc, ctx, 0, NULL);
    if (!th) return fn();   /* no thread available: fall back to inline probe */

    DWORD w = WaitForSingleObject(th, METRIC_PROBE_TIMEOUT_MS);
    CloseHandle(th);
    if (w == WAIT_OBJECT_0) return ctx->ok != 0;
    return false;           /* timed out: abandon, disable for the session */
}

void layer1_metrics_init(void)
{
    if (!s_metric_lock_ready) {
        InitializeCriticalSection(&s_metric_lock);
        s_metric_lock_ready = true;
    }
    memset(s_metrics, 0, sizeof(s_metrics));
    memset(s_metric_next_due, 0, sizeof(s_metric_next_due));

    uint64_t now = layer0_get_system_time_ms();
    s_metric_heartbeat_ms = now;

    for (int i = 0; i < METRIC_COUNT; i++) {
        const metric_provider_t *p = &k_providers[i];
        /* Only DLL-backed providers can wedge; kernel32 calls cannot. */
        bool needs_timeout = (i == METRIC_GPU_LOAD || i == METRIC_CPU_TEMP);

        if (i == METRIC_FAN_RPM) {
            /* Shares the hwmon shim already probed for CPU_TEMP: probing the
             * same DLL twice would double the worst-case startup stall. */
            s_provider_ok[i] = s_provider_ok[METRIC_CPU_TEMP];
        } else {
            s_provider_ok[i] = probe_bounded((metric_id_t)i, p->probe, needs_timeout);
        }

        s_metrics[i].state = s_provider_ok[i] ? MSTATE_WARMING : MSTATE_UNAVAILABLE;
        s_metrics[i].raw = s_metrics[i].quantized = s_metrics[i].aux = 0;
        s_metric_next_due[i] = now;   /* first poll immediately */

        printf("[LAYER 1 METRICS] %-9s <- %-24s %s\n",
               k_metric_label[i], p->name,
               s_provider_ok[i] ? "OK" : "UNAVAILABLE (tile renders N/A)");
    }
}

void layer1_metrics_poll(uint64_t now_ms)
{
    metric_sample_t staged[METRIC_COUNT];

    EnterCriticalSection(&s_metric_lock);
    memcpy(staged, s_metrics, sizeof(staged));
    LeaveCriticalSection(&s_metric_lock);

    for (int i = 0; i < METRIC_COUNT; i++) {
        if (!s_provider_ok[i]) continue;             /* permanently disabled */
        if (now_ms < s_metric_next_due[i]) continue; /* not due yet          */
        s_metric_next_due[i] = now_ms + k_providers[i].poll_period_ms;

        int32_t raw = 0, aux = 0;
        if (!k_providers[i].read(&raw, &aux)) {
            /* Transient miss: keep the last published value. Never fabricate,
             * and never downgrade an already-good tile to a fake reading. */
            continue;
        }
        staged[i].raw          = raw;
        staged[i].aux          = aux;
        staged[i].quantized    = quantize((metric_id_t)i, raw);
        staged[i].sample_count = staged[i].sample_count + 1u;
        staged[i].state        = MSTATE_OK;
    }

    EnterCriticalSection(&s_metric_lock);
    memcpy(s_metrics, staged, sizeof(s_metrics));
    s_metric_heartbeat_ms = now_ms;
    LeaveCriticalSection(&s_metric_lock);

    /* Hand the snapshot to the Layer 2 alarm engine, which owns hysteresis,
     * debounce and dirty-flag policy. */
    layer2_publish_metrics(staged);
}

void layer1_metrics_snapshot(metric_sample_t out[METRIC_COUNT])
{
    if (!out) return;
    EnterCriticalSection(&s_metric_lock);
    memcpy(out, s_metrics, sizeof(s_metrics));
    LeaveCriticalSection(&s_metric_lock);
}

uint64_t layer1_metrics_last_heartbeat_ms(void)
{
    uint64_t v;
    EnterCriticalSection(&s_metric_lock);
    v = s_metric_heartbeat_ms;
    LeaveCriticalSection(&s_metric_lock);
    return v;
}

void layer1_metrics_shutdown(void)
{
    /* Release every dynamically acquired handle, including for providers whose
     * probe failed: each release() is idempotent and safe on a partial probe. */
    for (int i = 0; i < METRIC_COUNT; i++) {
        if (k_providers[i].release) k_providers[i].release();
    }
    printf("[LAYER 1 METRICS] All dynamically loaded provider handles released.\n");
}

const char* layer1_metric_label(metric_id_t id)
{
    return (id < METRIC_COUNT) ? k_metric_label[id] : "?";
}

const char* layer1_metric_unit(metric_id_t id)
{
    return (id < METRIC_COUNT) ? k_metric_unit[id] : "";
}

const char* layer1_metric_provider_name(metric_id_t id)
{
    return (id < METRIC_COUNT) ? k_providers[id].name : "?";
}

bool layer1_metric_provider_available(metric_id_t id)
{
    return (id < METRIC_COUNT) ? s_provider_ok[id] : false;
}
