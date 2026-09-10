/**
 * @file main.c
 * @brief Prometheus99 HMI Runtime Application Entry Point
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 *
 * Orchestrates pure C99 4-Layer HMI runtime execution, native Win32 windowing,
 * 1000 Hz Sensor ISR thread, 30 Hz UI Presentation Loop, and Dual-Loop Watchdog.
 *
 * This file is the platform port: it owns the Win32 window and the display
 * driver (an 8bpp palettized DIB section that Layer 3 rasterizes straight into,
 * so there is exactly one copy of the pixel plane in the process).
 */

#include "../include/config.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer1_metrics.h"
#include "../include/layer2_core.h"
#include "../include/layer3_presentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <windows.h>
#include <process.h>

static HWND s_hwnd = NULL;
static volatile bool s_running = true; /* shared with the 1000 Hz ISR thread */
static HANDLE s_isr_thread = NULL;
static HANDLE s_metrics_thread = NULL;

/* --- Display driver state --- */
/* 4bpp DIB: the colour table is capped at 16 entries by the format itself. */
static struct {
    BITMAPINFOHEADER header;
    RGBQUAD          palette[PALETTE_MAX_SLOTS];
} s_bmi;
static HBITMAP s_dib = NULL;
static HDC     s_memdc = NULL;
static uint8_t *s_pixels = NULL;      /* GDI-owned; Layer 3 draws directly here */
static uint32_t s_palette_rev_seen = 0;

/* Upload Layer 3's palette into the DIB colour table. Switching theme costs
 * PAL_COUNT table entries -- the pixel plane never has to be re-rasterized. */
static void display_sync_palette(void)
{
    uint32_t rev = layer3_get_palette_revision();
    if (rev == s_palette_rev_seen) return;
    s_palette_rev_seen = rev;

    const uint32_t *pal = layer3_get_palette();
    RGBQUAD quads[PAL_COUNT];
    for (int i = 0; i < PAL_COUNT; i++) {
        quads[i].rgbRed      = (BYTE)((pal[i] >> 16) & 0xFF);
        quads[i].rgbGreen    = (BYTE)((pal[i] >> 8) & 0xFF);
        quads[i].rgbBlue     = (BYTE)(pal[i] & 0xFF);
        quads[i].rgbReserved = 0;
    }
    SetDIBColorTable(s_memdc, 0, PAL_COUNT, quads);
}

static bool display_init(void)
{
    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.header.biSize        = sizeof(BITMAPINFOHEADER);
    s_bmi.header.biWidth       = DISPLAY_WIDTH;
    /* One BAND tall, not one screen tall: this is the partial draw buffer. */
    s_bmi.header.biHeight      = -DISPLAY_BAND_HEIGHT; /* Top-down DIB */
    s_bmi.header.biPlanes      = 1;
    s_bmi.header.biBitCount    = DISPLAY_COLOR_DEPTH;
    s_bmi.header.biCompression = BI_RGB;
    s_bmi.header.biClrUsed     = PALETTE_MAX_SLOTS;
    s_bmi.header.biClrImportant= PAL_COUNT;

    s_memdc = CreateCompatibleDC(NULL);
    if (!s_memdc) return false;

    s_dib = CreateDIBSection(NULL, (BITMAPINFO *)&s_bmi, DIB_RGB_COLORS,
                             (void **)&s_pixels, NULL, 0);
    if (!s_dib || !s_pixels) return false;

    SelectObject(s_memdc, s_dib);
    return true;
}

/* Registered with Layer 1: pushes one finished band to the window. */
static HDC s_window_dc = NULL;

static void platform_display_flush(const display_area_t *area, const uint8_t *color_p)
{
    (void)color_p;   /* the band DIB is already the source surface */
    if (!s_window_dc) return;
    BitBlt(s_window_dc, 0, area->y1,
           DISPLAY_WIDTH, DISPLAY_BAND_HEIGHT,
           s_memdc, 0, 0, SRCCOPY);
}

static void display_shutdown(void)
{
    if (s_memdc) { DeleteDC(s_memdc); s_memdc = NULL; }
    if (s_dib)   { DeleteObject(s_dib); s_dib = NULL; }
    s_pixels = NULL;
}

/* Map a virtual key to a unified input action, or KEY_NONE. */
static input_key_t vk_to_input_key(WPARAM vk)
{
    switch (vk) {
        case VK_LEFT:   return KEY_PREV;
        case VK_RIGHT:  return KEY_NEXT;
        case VK_RETURN: return KEY_SELECT;
        case VK_ESCAPE: return KEY_BACK;
        case 'A':       return KEY_ALARM_ACK;
        case 'C':       return KEY_TOGGLE_CONTRAST;
        case 'F':
        case '6':       return KEY_TOGGLE_FAILOVER;
        default:        return KEY_NONE;
    }
}

/* Win32 Window Callback Handler */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_PAINT: {
            /* There is no full-screen backing store to blit from any more, so
             * validate the region and let the next UI tick (<=33 ms) repaint
             * every band. */
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            layer3_request_redraw();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; /* the blit covers every pixel; skip the flicker-inducing erase */

        case WM_LBUTTONDOWN:
            layer1_queue_touch_event((int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam), true);
            return 0;

        case WM_KEYDOWN: {
            /* Number keys 1-5 are direct screen shortcuts; everything else goes
             * through the HAL input queue and is drained by the 30 Hz tick. */
            if (wParam >= '1' && wParam <= '5') {
                layer2_fsm_request_screen_change((screen_id_t)(wParam - '1'));
                return 0;
            }
            input_key_t key = vk_to_input_key(wParam);
            if (key != KEY_NONE) layer1_queue_button_event(key);
            return 0;
        }

        case WM_DESTROY:
            s_running = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

/* High-Speed 1000 Hz Sensor ISR Background Thread */
static unsigned __stdcall sensor_isr_thread_proc(void *arg)
{
    (void)arg;
    while (s_running) {
        layer1_sensor_isr_handler_1000hz();
        Sleep(1); /* 1 ms = 1000 Hz */
    }
    return 0;
}

/*
 * Dedicated host-telemetry thread.
 *
 * Runs at 4 Hz and lets the provider engine decide which sources are actually
 * due (500 ms for kernel32 counters, 5 s for the PDH-backed GPU counter). The
 * Win32 telemetry APIs are called ONLY from here: never from the 1000 Hz ISR,
 * never from the UI render path.
 */
static unsigned __stdcall metrics_thread_proc(void *arg)
{
    (void)arg;
    while (s_running) {
        layer1_metrics_poll(layer0_get_system_time_ms());
        Sleep(250);
    }
    return 0;
}

/*
 * Return cold pages to the OS while the HMI is quiet.
 *
 * Because redraws are now gated on quantized change, a steady machine leaves
 * the 375 KB framebuffer untouched for long stretches. Trimming once per idle
 * period keeps the resident set near the code+stack floor instead of pinning
 * the whole framebuffer forever. Private commit is unaffected -- that has a
 * hard ~2 MB floor from the Win32 GUI subsystem itself.
 */
static void trim_working_set(void)
{
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
}

int main(int argc, char *argv[])
{
    bool want_gpu = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gpu") == 0) want_gpu = true;
    }

    /* Request 1 ms scheduler granularity so Sleep(1) in the sensor thread
     * actually approximates the advertised 1000 Hz cadence (winmm already linked). */
    timeBeginPeriod(1);

    printf("=================================================================\n");
    printf("     PROMETHEUS99: LIGHTWEIGHT HMI RUNTIME (C99 + LVGL SAFE)    \n");
    printf("     Team Doomsday: Abhilash L, Adarsh Abhilash, Nikhil Nuguri  \n");
    printf("=================================================================\n");
    printf("[INIT] Initializing 4-Layer Architecture...\n");

    if (!display_init()) {
        fprintf(stderr, "[ERROR] Failed to create the 8bpp display surface.\n");
        return 1;
    }
    printf("[INIT] Display %dx%d @ %d bpp indexed; banded draw buffer %d x %d = %d bytes.\n",
           DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_COLOR_DEPTH,
           DISPLAY_WIDTH, DISPLAY_BAND_HEIGHT, DISPLAY_BUF_SIZE);
    printf("[INIT] (a full 4bpp frame would cost %d bytes; %d bands per frame)\n",
           DISPLAY_FULLFRAME_SIZE, DISPLAY_BAND_COUNT);

    /* Initialize Layers */
    layer0_hardware_init();
    layer1_hal_init();
    layer2_core_init();
    layer1_metrics_enable_gpu(want_gpu);   /* PDH costs ~4.2 MB: opt-in only */
    layer1_metrics_init();          /* one-shot bounded probe of every provider */
    layer1_register_display_flush(platform_display_flush);
    layer3_presentation_init(s_pixels);
    display_sync_palette();

    /* Register Win32 Window Class */
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "Prometheus99HMIClass";
    RegisterClassExA(&wc);

    /* Calculate Window Rect for exact 800x480 client area */
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE;
    RECT rc = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    AdjustWindowRect(&rc, style, FALSE);

    s_hwnd = CreateWindowExA(
        0,
        "Prometheus99HMIClass",
        "Prometheus99: Lightweight HMI Runtime (C99 + LVGL)",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );

    if (!s_hwnd) {
        fprintf(stderr, "[ERROR] Failed to create Win32 HMI Window.\n");
        display_shutdown();
        return 1;
    }

    ShowWindow(s_hwnd, SW_SHOWNORMAL);
    UpdateWindow(s_hwnd);
    SetForegroundWindow(s_hwnd);
    SetFocus(s_hwnd);

    printf("[INIT] GUI Window Created (800x480 Resolution).\n");

    /* Launch 1000 Hz Sensor ISR Background Thread */
    s_isr_thread = (HANDLE)_beginthreadex(NULL, 0, sensor_isr_thread_proc, NULL, 0, NULL);
    if (!s_isr_thread) {
        fprintf(stderr, "[ERROR] Failed to start 1000 Hz Sensor ISR Thread.\n");
        display_shutdown();
        return 1;
    }

    s_metrics_thread = (HANDLE)_beginthreadex(NULL, 0, metrics_thread_proc, NULL, 0, NULL);
    if (!s_metrics_thread) {
        fprintf(stderr, "[ERROR] Failed to start host telemetry thread.\n");
        display_shutdown();
        return 1;
    }

    printf("[INIT] 1000 Hz Sensor Ingestion Thread Active.\n");
    printf("[INIT] Host Telemetry Poll Thread Active (%d ms fast / %d ms slow).\n",
           METRIC_POLL_PERIOD_MS, METRIC_SLOW_POLL_PERIOD_MS);
    printf("[INIT] 30 Hz LVGL UI Presentation Loop Started.\n");
    printf("-----------------------------------------------------------------\n");
    printf(" KEYBOARD SHORTCUTS:\n");
    printf("  [1] Boot Screen      [2] System Dashboard   [3] Binary Diagnostics\n");
    printf("  [4] Alarm Supervisor [5] Settings/Contrast  [6/F] Hot Standby Failover\n");
    printf("  [A] Acknowledge      [C] Toggle Contrast    [Esc] Back\n");
    printf("-----------------------------------------------------------------\n");

    /*
     * Main Execution Loop: 30 Hz UI tick + Win32 message pump.
     * The thread blocks in MsgWaitForMultipleObjects until either the next frame
     * deadline or an input message, instead of spinning on Sleep(5). That drops
     * roughly 170 pointless wakeups per second and makes frame pacing exact.
     */
    MSG msg;
    s_window_dc = GetDC(s_hwnd);
    uint64_t next_frame_ms = layer0_get_system_time_ms();
    uint64_t last_trim_ms = next_frame_ms;
#ifdef PROM_PROFILE
    uint64_t prof_at = next_frame_ms + 1000;
    unsigned prof_loops = 0, prof_frames = 0, prof_presents = 0;
#endif

    while (s_running) {
#ifdef PROM_PROFILE
        prof_loops++;
#endif
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!s_running) break;

        uint64_t now = layer0_get_system_time_ms();
#ifdef PROM_PROFILE
        if (now >= prof_at) {
            printf("[PROFILE] loops/s=%u  ticks/s=%u  presents/s=%u\n",
                   prof_loops, prof_frames, prof_presents);
            prof_loops = prof_frames = prof_presents = 0;
            prof_at = now + 1000;
        }
#endif
        if (now >= next_frame_ms) {
            next_frame_ms += UI_FRAME_PERIOD_MS;
            if (next_frame_ms <= now) {
                next_frame_ms = now + UI_FRAME_PERIOD_MS; /* recover after a stall */
            }

            /* Run 30 Hz Presentation Tick & Dual-Watchdog Check. Only present
             * when the tick reports the framebuffer actually changed. */
#ifdef PROM_PROFILE
            prof_frames++;
#endif
            display_sync_palette();
            if (layer3_ui_timer_tick_30hz()) {
#ifdef PROM_PROFILE
                prof_presents++;
#endif
            }

            /*
             * Periodic working-set trim.
             *
             * An idle-gap trigger does not work here: real host CPU load moves
             * by a whole percent most polls, so the UI legitimately redraws
             * about twice a second and the process is never "idle" for long.
             * A fixed cadence keeps the resident set near the code floor for
             * the cost of a handful of soft faults after each trim.
             */
            if ((now - last_trim_ms) >= WORKING_SET_TRIM_PERIOD_MS) {
                last_trim_ms = now;
                trim_working_set();
            }
            now = layer0_get_system_time_ms();
        }

        DWORD wait_ms = (next_frame_ms > now) ? (DWORD)(next_frame_ms - now) : 0;
        if (wait_ms > 0) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, wait_ms, QS_ALLINPUT);
        }
    }

    /* Cleanup */
    if (s_isr_thread) {
        WaitForSingleObject(s_isr_thread, 1000);
        CloseHandle(s_isr_thread);
    }
    if (s_metrics_thread) {
        WaitForSingleObject(s_metrics_thread, 2000);
        CloseHandle(s_metrics_thread);
    }
    layer1_metrics_shutdown();   /* releases every LoadLibrary/PDH handle */

    ReleaseDC(s_hwnd, s_window_dc);
    display_shutdown();
    timeEndPeriod(1);

    printf("[SHUTDOWN] Prometheus99 HMI Runtime Terminated Safely.\n");
    return 0;
}
