/* ===========================================================================
 * Host telemetry providers.
 *
 * Each provider is an independent unit behind the metric_provider_t vtable.
 * The engine never assumes a provider works: probe() decides once, at startup,
 * and a false result permanently parks that metric in MSTATE_UNAVAILABLE so its
 * tile renders "N/A" rather than a fabricated value.
 * ==========================================================================*/

/* --- Provider 1: CPU utilisation (GetSystemTimes, kernel32, always present) */

static ULONGLONG s_cpu_prev_idle, s_cpu_prev_total;
static bool      s_cpu_seeded;
static int32_t   s_cpu_core_count;

static ULONGLONG ft_to_u64(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static bool cpu_probe(void)
{
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return false;

    /* Seed the delta baseline from this very first real sample, so the first
     * published value is a true measurement and never a garbage-initialised
     * one. This is what keeps cold start from emitting a spurious alarm. */
    s_cpu_prev_idle  = ft_to_u64(&idle);
    s_cpu_prev_total = ft_to_u64(&kern) + ft_to_u64(&user);
    s_cpu_seeded = true;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s_cpu_core_count = (int32_t)si.dwNumberOfProcessors;
    return true;
}

static bool cpu_read(int32_t *out, int32_t *aux)
{
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return false;

    ULONGLONG i = ft_to_u64(&idle);
    ULONGLONG t = ft_to_u64(&kern) + ft_to_u64(&user);
    if (!s_cpu_seeded) {
        s_cpu_prev_idle = i; s_cpu_prev_total = t; s_cpu_seeded = true;
        return false;                       /* baseline only, not a reading */
    }

    ULONGLONG dt = t - s_cpu_prev_total;
    ULONGLONG di = i - s_cpu_prev_idle;
    s_cpu_prev_idle  = i;
    s_cpu_prev_total = t;
    if (dt == 0) return false;              /* no elapsed time: not a reading */
    if (di > dt) di = dt;

    *out = (int32_t)(1000ULL - (di * 1000ULL) / dt);  /* tenths of a percent */
    *aux = s_cpu_core_count;
    return true;
}

/* --- Provider 2: Physical memory (GlobalMemoryStatusEx, kernel32) --------- */

static bool mem_probe(void)
{
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    return GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0;
}

static bool mem_read(int32_t *out, int32_t *aux)
{
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0) return false;
    *out = (int32_t)(ms.dwMemoryLoad * 10u);                 /* tenths of a % */
    *aux = (int32_t)(ms.ullTotalPhys / (1024ULL * 1024ULL)); /* total MB      */
    return true;
}

/* --- Provider 3: Battery / power state (GetSystemPowerStatus, kernel32) --- */

static bool batt_probe(void)
{
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return false;
    /* 255 means "unknown": a desktop with no battery. Report unavailable
     * rather than inventing a charge level. */
    return sps.BatteryLifePercent != 255;
}

static bool batt_read(int32_t *out, int32_t *aux)
{
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return false;
    if (sps.BatteryLifePercent == 255) return false;
    *out = (int32_t)sps.BatteryLifePercent * 10;
    *aux = (int32_t)sps.ACLineStatus;   /* 1 = on mains, 0 = on battery */
    return true;
}

/* --- Provider 4: GPU utilisation (PDH, dynamically loaded) ----------------
 * pdh.dll is resolved with LoadLibrary/GetProcAddress rather than statically
 * linked, so a machine without PDH costs the base binary nothing and can never
 * fail to start. Values are pulled as PDH_FMT_LONG to keep the telemetry path
 * integer-only.
 */

typedef PDH_STATUS (WINAPI *pfn_PdhOpenQueryA)(LPCSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS (WINAPI *pfn_PdhAddEnglishCounterA)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS (WINAPI *pfn_PdhCollectQueryData)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *pfn_PdhGetFormattedCounterArrayA)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_A);
typedef PDH_STATUS (WINAPI *pfn_PdhCloseQuery)(PDH_HQUERY);

static HMODULE                          s_pdh_lib;
static pfn_PdhOpenQueryA                s_PdhOpenQueryA;
static pfn_PdhAddEnglishCounterA        s_PdhAddEnglishCounterA;
static pfn_PdhCollectQueryData          s_PdhCollectQueryData;
static pfn_PdhGetFormattedCounterArrayA s_PdhGetFormattedCounterArrayA;
static pfn_PdhCloseQuery                s_PdhCloseQuery;
static PDH_HQUERY                       s_gpu_query;
static PDH_HCOUNTER                     s_gpu_counter;

/*
 * Scratch for the wildcard counter array.
 *
 * Measured on a real desktop: the UNFILTERED "GPU Engine(*)" wildcard expands
 * to 559 instances needing 48,993 bytes, so the original 8 KB buffer silently
 * failed every single poll. Filtered to engtype_3D it is 127 instances /
 * 10,733 bytes, so 16 KB carries real headroom.
 *
 * It is a single lazy page reservation rather than a static array: a static
 * 16 KB buffer lands in PE .data and would cost 16 KB of BINARY for a provider
 * that is off by default. This is one VirtualAlloc at probe and one VirtualFree
 * at release -- not a heap allocation, so the zero-fragmentation property of
 * the runtime is preserved.
 */
#define PDH_SCRATCH_BYTES 16384
static unsigned char *s_pdh_scratch;
static bool           s_pdh_overflow_logged;

static void gpu_release(void)
{
    if (s_gpu_query && s_PdhCloseQuery) s_PdhCloseQuery(s_gpu_query);
    if (s_pdh_scratch) { VirtualFree(s_pdh_scratch, 0, MEM_RELEASE); s_pdh_scratch = NULL; }
    s_gpu_query   = NULL;
    s_gpu_counter = NULL;
    if (s_pdh_lib) FreeLibrary(s_pdh_lib);
    s_pdh_lib = NULL;
    s_PdhOpenQueryA = NULL; s_PdhAddEnglishCounterA = NULL;
    s_PdhCollectQueryData = NULL; s_PdhGetFormattedCounterArrayA = NULL;
    s_PdhCloseQuery = NULL;
}

/*
 * PDH is OPT-IN.
 *
 * Measured cost of the GPU counter path on a real desktop: pdh.dll itself is
 * +232 KB, but PdhAddEnglishCounter on a wildcard instance set is +3.4 MB and
 * the formatted array another +560 KB -- about 4.2 MB of private commit, far
 * more than everything else in this runtime combined. That is a real trade,
 * so the operator makes it explicitly with --gpu rather than paying it by
 * default. Without the flag the tile renders N/A like any other unavailable
 * provider.
 */
static bool s_gpu_enabled = false;

void layer1_metrics_enable_gpu(bool enable)
{
    s_gpu_enabled = enable;
}

static bool gpu_probe(void)
{
    if (!s_gpu_enabled) return false;
    s_pdh_lib = LoadLibraryA("pdh.dll");
    if (!s_pdh_lib) return false;

    s_pdh_scratch = (unsigned char *)VirtualAlloc(NULL, PDH_SCRATCH_BYTES,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_pdh_scratch) { gpu_release(); return false; }

    s_PdhOpenQueryA                = (pfn_PdhOpenQueryA)(void*)GetProcAddress(s_pdh_lib, "PdhOpenQueryA");
    s_PdhAddEnglishCounterA        = (pfn_PdhAddEnglishCounterA)(void*)GetProcAddress(s_pdh_lib, "PdhAddEnglishCounterA");
    s_PdhCollectQueryData          = (pfn_PdhCollectQueryData)(void*)GetProcAddress(s_pdh_lib, "PdhCollectQueryData");
    s_PdhGetFormattedCounterArrayA = (pfn_PdhGetFormattedCounterArrayA)(void*)GetProcAddress(s_pdh_lib, "PdhGetFormattedCounterArrayA");
    s_PdhCloseQuery                = (pfn_PdhCloseQuery)(void*)GetProcAddress(s_pdh_lib, "PdhCloseQuery");

    if (!s_PdhOpenQueryA || !s_PdhAddEnglishCounterA || !s_PdhCollectQueryData ||
        !s_PdhGetFormattedCounterArrayA || !s_PdhCloseQuery) { gpu_release(); return false; }

    if (s_PdhOpenQueryA(NULL, 0, &s_gpu_query) != ERROR_SUCCESS) { gpu_release(); return false; }

    /* English counter path: unaffected by the machine's display language. */
    if (s_PdhAddEnglishCounterA(s_gpu_query, "\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                0, &s_gpu_counter) != ERROR_SUCCESS) { gpu_release(); return false; }

    /* A rate counter needs a baseline sample before it can format. */
    if (s_PdhCollectQueryData(s_gpu_query) != ERROR_SUCCESS) { gpu_release(); return false; }
    return true;
}

static bool gpu_read(int32_t *out, int32_t *aux)
{
    if (!s_gpu_query || !s_pdh_scratch) return false;
    if (s_PdhCollectQueryData(s_gpu_query) != ERROR_SUCCESS) return false;

    DWORD size  = (DWORD)PDH_SCRATCH_BYTES;
    DWORD count = 0;
    PDH_STATUS st = s_PdhGetFormattedCounterArrayA(
        s_gpu_counter, PDH_FMT_LONG, &size, &count,
        (PPDH_FMT_COUNTERVALUE_ITEM_A)s_pdh_scratch);

    if (st == (PDH_STATUS)PDH_MORE_DATA) {
        /* Never fail silently again: say so once, then keep reporting a miss so
         * the tile holds its last good value instead of inventing one. */
        if (!s_pdh_overflow_logged) {
            s_pdh_overflow_logged = true;
            printf("[LAYER 1 METRICS] GPU counter array needs %lu bytes, scratch is %d -- "
                   "raise PDH_SCRATCH_BYTES.\n", (unsigned long)size, PDH_SCRATCH_BYTES);
        }
        return false;
    }
    if (st != ERROR_SUCCESS || count == 0) return false;

    const PDH_FMT_COUNTERVALUE_ITEM_A *items = (const PDH_FMT_COUNTERVALUE_ITEM_A *)s_pdh_scratch;
    long total = 0;
    for (DWORD i = 0; i < count; i++) {
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS) total += items[i].FmtValue.longValue;
    }
    if (total < 0)   total = 0;
    if (total > 100) total = 100;     /* engine utilisations sum past 100 */
    *out = (int32_t)(total * 10);     /* tenths of a percent */
    *aux = (int32_t)count;            /* engine instances observed */
    return true;
}

/* --- Provider 5: CPU temperature / fan (optional, HAVE_HWMON) -------------
 * Stock Win32 exposes NO reliable CPU temperature or fan tachometer API.
 * ACPI thermal zones via WMI are absent or meaningless on most desktops, and
 * dragging COM/WMI in would cost more binary than the rest of this runtime.
 *
 * So this is an explicitly optional provider. Built with -DHAVE_HWMON it looks
 * for a side-loaded shim DLL (prom_hwmon.dll) exporting
 *     int prom_hwmon_read(int32_t *deci_celsius, int32_t *fan_rpm);
 * returning non-zero on success -- the natural integration point for a vendor
 * SDK or a LibreHardwareMonitor bridge. Without the DLL, or built without the
 * flag, both tiles render "N/A". They are never fabricated.
 */

#ifdef HAVE_HWMON
typedef int (WINAPI *pfn_hwmon_read)(int32_t *, int32_t *);
static HMODULE        s_hwmon_lib;
static pfn_hwmon_read s_hwmon_read;

static void hwmon_release(void)
{
    if (s_hwmon_lib) FreeLibrary(s_hwmon_lib);
    s_hwmon_lib  = NULL;
    s_hwmon_read = NULL;
}

static bool hwmon_probe(void)
{
    s_hwmon_lib = LoadLibraryA("prom_hwmon.dll");
    if (!s_hwmon_lib) return false;
    s_hwmon_read = (pfn_hwmon_read)(void*)GetProcAddress(s_hwmon_lib, "prom_hwmon_read");
    if (!s_hwmon_read) { hwmon_release(); return false; }
    int32_t t = 0, f = 0;
    if (!s_hwmon_read(&t, &f)) { hwmon_release(); return false; }
    return true;
}

static bool hwmon_read_temp(int32_t *out, int32_t *aux)
{
    int32_t t = 0, f = 0;
    if (!s_hwmon_read || !s_hwmon_read(&t, &f)) return false;
    *out = t; *aux = 0; return true;
}

static bool hwmon_read_fan(int32_t *out, int32_t *aux)
{
    int32_t t = 0, f = 0;
    if (!s_hwmon_read || !s_hwmon_read(&t, &f)) return false;
    *out = f; *aux = 0; return true;
}
#else
/* Compiled out entirely: costs no code, no import, no runtime probe attempt. */
static bool hwmon_probe(void)                       { return false; }
static bool hwmon_read_temp(int32_t *o, int32_t *a) { (void)o; (void)a; return false; }
static bool hwmon_read_fan(int32_t *o, int32_t *a)  { (void)o; (void)a; return false; }
static void hwmon_release(void)                     { }
#endif
