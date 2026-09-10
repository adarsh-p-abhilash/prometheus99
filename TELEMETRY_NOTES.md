# Prometheus99 — Live Host Telemetry Conversion

Before/after note for the change from synthetic PLC vitals to real host telemetry
with a hysteresis/debounce alarm engine.

All numbers below are measured on this machine, not estimated.

---

## 1. Alarm spam — the headline result

Reproduce with `alarm_bench` (includes `layer2_core.c` verbatim, so the "after"
column is the shipping engine, not a mock-up).

| Test (60 s window) | BEFORE | AFTER |
|---|---:|---:|
| **A.** Old random generator @1000 Hz, single-value thresholds — alarm transitions | **35 / min** | n/a |
| **A.** …and UI dirty marks | **60,000 / min** | n/a |
| **B.** CPU oscillating 84.0–86.0 % across an 85.0 % limit — alarm events | **60 / min** | **1 / min** |
| **C.** Real host CPU (3.7 – 30.2 %, mean 15.4 %) — alarm events | 0 / min | 0 / min |
| **C.** Real host CPU — UI re-rasterizations | **60,000 / min** | **110 / min** |

The pathological case (B) is the point: a value hovering on the threshold used to
toggle the alarm on **every** crossing — 60 events a minute. With an 85.0 % arm /
75.0 % clear band and a 3-sample confirm, it produces **one** event: the initial
arm, then silence.

Redraws dropped **545×** (60,000 → 110 per minute) because the UI is now gated on
*quantized* change, not on "a poll happened".

### Why it works

1. **Hysteresis band, not a point.** `arm` and `clear` are separate values with a
   dead zone between them (`k_alarm_rules` in `layer2_core.c`).
2. **N-sample debounce.** A breach must persist `confirm_samples` consecutive
   polls before the FSM moves. Single-sample spikes never reach it.
3. **Quantize before compare.** Raw values are rounded to display precision
   (whole percent / whole degree / 50 RPM) *before* diffing, so sub-display noise
   cannot mark anything dirty.
4. **Edge-triggered, not level-triggered.** One latched event per confirmed
   transition. Holding a condition emits nothing further.
5. **Decoupled rates.** 1000 Hz ISR (liveness only) / 2 Hz metric poll / 30 Hz UI
   timer / redraw only on change. The old design let a 1000 Hz loop drive the UI.

---

## 2. Binary size

| Build | Size |
|---|---:|
| Before (`-O2`, full-frame 8bpp) | 51.5 KB |
| **After** (`-Os`, 4bpp banded, live telemetry) | **45.0 KB** |

`-Os` beat `-O2` by ~20 KB here and the rasterizer has ~30× headroom against the
33 ms frame budget, so size wins. Flags: `-Os -ffunction-sections -fdata-sections
-fno-asynchronous-unwind-tables -fno-unwind-tables -s -Wl,--gc-sections`.

The PDH scratch buffer is a lazy `VirtualAlloc` rather than a static array
specifically because a static 16 KB buffer lands in PE `.data` and would cost
16 KB of *binary* for a provider that is off by default.

---

## 3. Memory

| Metric | Before | After |
|---|---:|---:|
| Pixel buffer | 375 KB (full frame, 8bpp) | **19.2 KB** (one 800×48 band, 4bpp) |
| Working set — min | 13,800 KB | **328 KB** |
| Working set — mean | 13,800 KB | **460 KB** |
| Private commit | 3,520 KB | **2,092 KB** |
| **App-attributable** | ~1,564 KB | **128 KB** |

**App-attributable** = private commit minus the measured bare-Win32-GUI floor.

### The floor is real and is not our code

Measured in-process on an empty window that draws nothing:

| Stage | Working Set | Private |
|---|---:|---:|
| bare process | 4,804 KB | 864 KB |
| + `CreateWindow` + GDI init | 13,224 KB | **1,964 KB** |
| after working-set trim | 256 KB | 1,964 KB |

`user32` + `gdi32` + UCRT cost ~2.0 MB of private commit before a single line of
application code runs. **No app-side change can bring private commit under
500 KB**, let alone 150 KB. What *is* controllable — our own buffers — is now
128 KB.

Two levers got us there:

- **Banded rendering.** There is no full-screen framebuffer. The logical surface
  is still 800×480, but it is rasterized one 48-row strip at a time into a single
  19.2 KB buffer that is blitted immediately (LVGL's partial-draw-buffer model).
  `draw_rect` does the band translation; all drawing code still works in
  full-screen coordinates.
- **Periodic working-set trim** every 3 s. An idle-gap trigger does *not* work:
  real CPU load moves a whole percent most polls, so the UI legitimately redraws
  ~2×/s and the process is never idle for long.

---

## 4. Two real bugs found while measuring

**The GPU provider never worked.** `PdhGetFormattedCounterArrayA` on the
unfiltered `\GPU Engine(*)` wildcard expands to **559 instances needing 48,993
bytes**. The scratch buffer was 8,192 — so it returned `PDH_MORE_DATA` and the
provider silently failed *every poll*. Now: filtered to `engtype_3D` (127
instances / 10,733 bytes), 16 KB buffer, and `PDH_MORE_DATA` is logged once
rather than swallowed.

**PDH costs 4.2 MB.** Measured step by step:

| Step | Private delta |
|---|---:|
| `LoadLibrary("pdh.dll")` | +232 KB |
| `PdhAddEnglishCounter` (wildcard instance set) | **+3,452 KB** |
| Collect + format | +560 KB |

That is more than the entire rest of the runtime combined, so the GPU provider is
**opt-in**: run with `--gpu`. Without it the tile renders `N/A` like any other
unavailable provider. With it, private commit goes 2,092 KB → 5,716 KB.

---

## 5. Robustness details

- **Cold start.** The debounce window is seeded from the *first real sample*
  (`cpu_probe` captures the `GetSystemTimes` baseline; `alarm_fsm.seeded` gates
  evaluation). The committed level stays `ALARM_NONE` until `confirm_samples`
  agreeing samples exist, so no alarm can fire before there is data to justify it.
- **Bounded probes.** DLL-backed providers are probed once on a throwaway thread
  with a 1,500 ms timeout. A timeout disables that provider for the session — it
  is never retried per poll. The orphan thread is *abandoned*, not
  `TerminateThread`-ed, because killing a thread mid-`LoadLibrary` leaves the
  loader lock held and deadlocks the process.
- **Handle release.** `layer1_metrics_shutdown()` calls every provider's
  `release()` on the way out, including providers whose probe failed; each is
  idempotent and safe on a partial probe.
- **Unavailable is a visual state.** Dimmed border, literal `N/A`, the reason
  (`PROVIDER UNAVAILABLE`) and the provider name. Never blank, never a stale
  value presented as live. A transient read failure holds the last good value
  and does *not* downgrade the tile.
- **Watchdog sees the poll loop.** The dual-loop AND check is now a **triple**
  loop: ISR (1000 Hz / 500 ms window), UI (30 Hz / 500 ms window), metric poll
  (2 Hz / 4,000 ms window — its own cadence, or it would read as dead always).
  A hung PDH or WMI call is now visible instead of silent.
- **Correlated-symptom suppression.** `CPU TEMP` declares `CPU LOAD` as its root
  cause and `FAN` declares `CPU TEMP`. When a root is already in alarm, the
  dependent transition is logged as `[SYMPTOM]`, excluded from overall severity,
  and shown as `(SYMPTOM)` on its tile rather than raising an independent alarm.

---

## 6. Honest gaps

- **CPU temperature and fan RPM have no stock Win32 API.** They are an optional
  provider (`-DHAVE_HWMON`) that side-loads `prom_hwmon.dll` exporting
  `int prom_hwmon_read(int32_t *deci_celsius, int32_t *fan_rpm)` — the natural
  integration point for a vendor SDK or LibreHardwareMonitor bridge. Without it
  both tiles render `N/A`. Nothing is ever fabricated.
- **There is no LVGL in this project.** The presentation layer is a bespoke
  integer software rasterizer that follows LVGL's structural conventions
  (pre-allocated static screens, partial draw buffer, flush callback). The
  filenames and comments say "LVGL" for historical reasons.
- **110 redraws/min is honest, not floor.** On an active machine the whole-percent
  CPU value genuinely changes most polls. Coarsening `k_metric_quantum` to 2 %
  steps would roughly halve it, at the cost of display precision.
- `-Wpedantic` flags the `GetProcAddress` function-pointer casts as ISO C
  violations. That is inherent to the Win32 dynamic-loading idiom; the normal
  `-Wall -Wextra` build is clean.
