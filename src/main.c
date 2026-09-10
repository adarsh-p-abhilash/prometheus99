/**
 * @file main.c
 * @brief Prometheus99 HMI Runtime Application Entry Point
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 * 
 * Orchestrates pure C99 4-Layer HMI runtime execution, native Win32 windowing,
 * 1000 Hz Sensor ISR thread, 30 Hz UI Presentation Loop, and Dual-Loop Watchdog.
 */

#include "../include/config.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include "../include/layer3_presentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <process.h>

static HWND s_hwnd = NULL;
static struct {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[16];
} s_bmi_strip;
static HDC s_active_paint_hdc = NULL;
static bool s_running = true;
static HANDLE s_isr_thread = NULL;

void main_flush_strip_to_screen(const display_area_t *area, const uint8_t *color_p)
{
    HDC hdc = s_active_paint_hdc;
    bool release_dc = false;
    if (!hdc && s_hwnd) {
        hdc = GetDC(s_hwnd);
        release_dc = true;
    }
    if (hdc && area && color_p) {
        int w = area->x2 - area->x1 + 1;
        int h = area->y2 - area->y1 + 1;
        StretchDIBits(hdc,
                      area->x1, area->y1, w, h,
                      0, 0, w, h,
                      color_p, (const BITMAPINFO*)&s_bmi_strip, DIB_RGB_COLORS, SRCCOPY);
    }
    if (release_dc && hdc && s_hwnd) {
        ReleaseDC(s_hwnd, hdc);
    }
}

/* Win32 Window Callback Handler */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE:
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            s_active_paint_hdc = hdc;
            layer3_ui_timer_tick_30hz();
            s_active_paint_hdc = NULL;
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            layer3_handle_touch((int16_t)x, (int16_t)y, true);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            switch (wParam) {
                case '1':
                    layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
                    break;
                case '2':
                    layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
                    break;
                case '3':
                    layer2_fsm_request_screen_change(SCREEN_ALARM);
                    break;
                case '4':
                    layer2_fsm_request_screen_change(SCREEN_SETTINGS);
                    break;
                case '5':
                case '6':
                case 'F':
                    layer3_inject_input_key(KEY_TOGGLE_FAILOVER);
                    break;
                case 'A':
                    layer3_inject_input_key(KEY_ALARM_ACK);
                    break;
                case 'C':
                    layer3_inject_input_key(KEY_TOGGLE_CONTRAST);
                    break;
                case VK_LEFT:
                    layer2_fsm_process_event(KEY_PREV);
                    break;
                case VK_RIGHT:
                    layer2_fsm_process_event(KEY_NEXT);
                    break;
                case VK_RETURN:
                    layer2_fsm_process_event(KEY_SELECT);
                    break;
                case VK_ESCAPE:
                    layer2_fsm_process_event(KEY_BACK);
                    break;
                default:
                    break;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DESTROY:
            s_running = false;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=================================================================\n");
    printf("     PROMETHEUS99: LIGHTWEIGHT HMI RUNTIME (C99 + LVGL SAFE)    \n");
    printf("     Team Doomsday: Abhilash L, Adarsh Abhilash, Nikhil Nuguri  \n");
    printf("=================================================================\n");
    printf("[INIT] Initializing 4-Layer Architecture...\n");

    /* Initialize Layers */
    layer0_hardware_init();
    layer1_hal_init();
    layer2_core_init();
    layer3_presentation_init();

    /* Setup Win32 Bitmap Format for 800x30 4-Bit Strip Palette Rendering */
    memset(&s_bmi_strip, 0, sizeof(s_bmi_strip));
    s_bmi_strip.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi_strip.bmiHeader.biWidth = DISPLAY_WIDTH;
    s_bmi_strip.bmiHeader.biHeight = -STRIP_HEIGHT; /* Top-down Strip DIB */
    s_bmi_strip.bmiHeader.biPlanes = 1;
    s_bmi_strip.bmiHeader.biBitCount = 4;
    s_bmi_strip.bmiHeader.biCompression = BI_RGB;
    s_bmi_strip.bmiHeader.biClrUsed = 16;
    s_bmi_strip.bmiHeader.biClrImportant = 16;

    /* Standard 16-Color HMI Palette Table (RGBQUAD: Blue, Green, Red, Reserved) */
    static const RGBQUAD palette16[16] = {
        {0x00, 0x00, 0x00, 0}, /* 0: Black */
        {0x2A, 0x17, 0x0F, 0}, /* 1: Dark Navy */
        {0x3B, 0x29, 0x1E, 0}, /* 2: Slate Card */
        {0x58, 0x3A, 0x1E, 0}, /* 3: Dark Cyan Grid */
        {0x89, 0x4E, 0x1D, 0}, /* 4: Dark Blue */
        {0xF6, 0x82, 0x3B, 0}, /* 5: Electric Blue */
        {0xFF, 0xD2, 0x00, 0}, /* 6: Glowing Cyan */
        {0x81, 0xB9, 0x10, 0}, /* 7: Emerald Green */
        {0x66, 0xFF, 0x00, 0}, /* 8: Bright Green */
        {0x44, 0x44, 0xEF, 0}, /* 9: Crimson Red */
        {0x00, 0x00, 0xFF, 0}, /* 10: Bright Red */
        {0x07, 0xC1, 0xFF, 0}, /* 11: Amber / Yellow */
        {0x00, 0xFF, 0xFF, 0}, /* 12: Vibrant Yellow */
        {0xB8, 0xA3, 0x94, 0}, /* 13: Cool Grey */
        {0xF0, 0xE8, 0xE2, 0}, /* 14: Light Grey */
        {0xFF, 0xFF, 0xFF, 0}  /* 15: Pure White */
    };
    memcpy(s_bmi_strip.bmiColors, palette16, sizeof(palette16));

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
    RECT rc = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);

    s_hwnd = CreateWindowExA(
        0,
        "Prometheus99HMIClass",
        "Prometheus99: Lightweight HMI Runtime (C99 + LVGL)",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL
    );

    if (!s_hwnd) {
        fprintf(stderr, "[ERROR] Failed to create Win32 HMI Window.\n");
        return 1;
    }

    ShowWindow(s_hwnd, SW_SHOWNORMAL);
    UpdateWindow(s_hwnd);
    SetForegroundWindow(s_hwnd);
    SetFocus(s_hwnd);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    printf("[INIT] GUI Window Created (800x480 Resolution).\n");

    /* Launch 1000 Hz Sensor ISR Background Thread with 8KB stack (RAM optimized) */
    s_isr_thread = (HANDLE)_beginthreadex(NULL, 8192, sensor_isr_thread_proc, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!s_isr_thread) {
        fprintf(stderr, "[ERROR] Failed to start 1000 Hz Sensor ISR Thread.\n");
        return 1;
    }

    printf("[INIT] 1000 Hz Sensor Ingestion Thread Active.\n");
    printf("[INIT] 30 Hz LVGL UI Presentation Loop Started.\n");
    printf("-----------------------------------------------------------------\n");
    printf(" KEYBOARD SHORTCUTS:\n");
    printf("  [1] System Dashboard   [2] Binary Diagnostics [3] Alarm Supervisor\n");
    printf("  [4] Settings/Contrast  [5/F] Hot Standby Failover\n");
    printf("  [A] Acknowledge      [C] Toggle Contrast    [Esc] Back\n");
    printf("-----------------------------------------------------------------\n");

    /* Main Execution Loop: 30 Hz UI Timer Tick + Win32 Message Pump */
    MSG msg;
    uint64_t last_ui_tick = layer0_get_system_time_ms();

    while (s_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        uint64_t now = layer0_get_system_time_ms();
        if ((now - last_ui_tick) >= UI_FRAME_PERIOD_MS) {
            last_ui_tick = now;

            /* Run 30 Hz LVGL Presentation Tick & Dual-Watchdog Check */
            layer3_ui_timer_tick_30hz();

            /* Trigger Redraw on Window */
            InvalidateRect(s_hwnd, NULL, FALSE);

            /* Periodically trim OS cached working set pages */
            static uint32_t frame_count = 0;
            if (++frame_count % 30 == 0) {
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
        }

        Sleep(5); /* Yield CPU */
    }

    /* Cleanup */
    if (s_isr_thread) {
        WaitForSingleObject(s_isr_thread, 1000);
        CloseHandle(s_isr_thread);
    }

    printf("[SHUTDOWN] Prometheus99 HMI Runtime Terminated Safely.\n");
    return 0;
}
