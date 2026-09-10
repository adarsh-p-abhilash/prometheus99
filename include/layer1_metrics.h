/**
 * @file layer1_metrics.h
 * @brief Layer 1: Host Telemetry Metric Providers
 *
 * Replaces the synthetic random PLC generator with real host vitals.
 *
 * Design contract:
 *  - Every metric is produced by a provider exposing a small vtable, so an
 *    unavailable sensor is a clean MSTATE_UNAVAILABLE fallback, never a crash
 *    and never a fabricated number.
 *  - Providers are probed ONCE at startup with a bounded timeout. A provider
 *    that fails probe is disabled for the whole session; it is never retried
 *    per poll cycle.
 *  - Values are scaled integers. There is no floating point anywhere in the
 *    telemetry or alarm path.
 *  - Providers are polled from a dedicated low-rate thread, never from the
 *    1000 Hz ISR and never from the UI render path.
 */

#ifndef LAYER1_METRICS_H
#define LAYER1_METRICS_H

#include "config.h"

/* --- Metric identity ------------------------------------------------------
 * scale column documents the fixed-point unit stored in metric_sample_t.raw
 */
typedef enum {
    METRIC_CPU_LOAD = 0,  /* tenths of a percent   0..1000  */
    METRIC_MEM_USED,      /* tenths of a percent   0..1000  */
    METRIC_GPU_LOAD,      /* tenths of a percent   0..1000  */
    METRIC_BATTERY,       /* tenths of a percent   0..1000  */
    METRIC_CPU_TEMP,      /* deci-Celsius          0..1500  */
    METRIC_FAN_RPM,       /* RPM                            */
    METRIC_COUNT
} metric_id_t;

/* --- Availability state --------------------------------------------------- */
typedef enum {
    MSTATE_UNAVAILABLE = 0, /* probe failed: tile renders a fixed "N/A" forever */
    MSTATE_WARMING,         /* provider live, debounce window not yet seeded    */
    MSTATE_OK               /* provider live, value trustworthy                 */
} metric_state_t;

/* --- One published metric -------------------------------------------------
 * quantized is raw rounded to the display's real precision. Dirty detection
 * compares quantized values only, so sub-display-resolution jitter can never
 * mark the UI dirty.
 */
typedef struct {
    int32_t        raw;        /* provider units (see metric_id_t)      */
    int32_t        quantized;  /* rounded to display precision          */
    int32_t        aux;        /* context, e.g. total RAM in MB         */
    metric_state_t state;
    uint32_t       sample_count;
} metric_sample_t;

/* --- Provider vtable ------------------------------------------------------
 * probe():  one-shot, bounded, at startup. false => disabled for the session.
 * read():   fill *out in the metric's scaled units. false => transient miss,
 *           the previous published value is retained (never fabricated).
 * release():release any dynamically loaded handle. Always called on shutdown,
 *           including for providers whose probe() failed.
 */
typedef struct metric_provider {
    const char *name;
    metric_id_t metric;
    uint16_t    poll_period_ms;
    bool      (*probe)(void);
    bool      (*read)(int32_t *out, int32_t *aux);
    void      (*release)(void);
} metric_provider_t;

/* --- Engine API ----------------------------------------------------------- */
/* Opt in to the PDH-backed GPU provider. Must be called BEFORE
 * layer1_metrics_init(). Off by default: see the cost note in the providers. */
void  layer1_metrics_enable_gpu(bool enable);
void  layer1_metrics_init(void);            /* probes every provider once     */
void  layer1_metrics_poll(uint64_t now_ms); /* called by the metric thread    */
void  layer1_metrics_shutdown(void);        /* releases every handle          */

/* Thread-safe snapshot of all metrics for the UI / alarm engine. */
void  layer1_metrics_snapshot(metric_sample_t out[METRIC_COUNT]);

/* Liveness beacon so a hung provider is visible to the dual-loop watchdog. */
uint64_t layer1_metrics_last_heartbeat_ms(void);

/* Introspection for the diagnostics screen. */
const char* layer1_metric_label(metric_id_t id);
const char* layer1_metric_unit(metric_id_t id);
const char* layer1_metric_provider_name(metric_id_t id);
bool        layer1_metric_provider_available(metric_id_t id);

#endif /* LAYER1_METRICS_H */
