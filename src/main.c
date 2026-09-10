/**
 * @file main.c
 * @brief Prometheus99 HMI Runtime Application Entry Point
 * @architect Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)
 * 
 * Orchestrates pure C99 4-Layer HMI runtime execution, native Win32 windowing,
 * 1000 Hz Sensor ISR thread, 30 Hz UI Presentation Loop, and Dual-Loop Watchdog.
 *
 * Changes from original:
 *   - Action keys (6/F, A, C, arrows, Enter, Esc) routed through Layer 1 HAL queue.
 *   - Number keys (1-5) do direct screen jumps only (no conflicting queue event).
 *   - Touch input routed through Layer 1 HAL queue.
 *   - Proper window class unregistration on shutdown.
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
static BITMAPINFO s_bmi;
static bool s_running = true;
static HANDLE s_isr_thread = NULL;
static const char *S_WND_CLASS = "Prometheus99HMIClass";

/* Win32 Window Callback Handler */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE:
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            const uint32_t *fb = layer3_get_framebuffer();
            if (fb) {
                StretchDIBits(hdc,
                              0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                              0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                              fb, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            /* Route through Layer 1 HAL queue */
            layer1_queue_touch((int16_t)x, (int16_t)y, true);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            switch (wParam) {
                /* Number keys 1-5: DIRECT screen jumps only.
                 * Do NOT also queue a HAL navigation event — that would cause
                 * the FSM to navigate AWAY from the selected screen on the
                 * next tick (e.g., pressing '2' would go to Dashboard, then
                 * KEY_NEXT in the queue would move to Diagnostics). */
                case '1':
                    layer2_fsm_request_screen_change(SCREEN_BOOT);
                    break;
                case '2':
                    layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
                    break;
                case '3':
                    layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
                    break;
                case '4':
                    layer2_fsm_request_screen_change(SCREEN_ALARM);
                    break;
                case '5':
                    layer2_fsm_request_screen_change(SCREEN_SETTINGS);
                    break;

                /* Action keys: route through Layer 1 HAL queue.
                 * These are processed by the presentation tick handler
                 * via layer1_poll_button_event() -> layer3_inject_input_key(). */
                case '6':
                case 'F':
                    layer1_queue_key(KEY_TOGGLE_FAILOVER);
                    break;
                case 'A':
                    layer1_queue_key(KEY_ALARM_ACK);
                    break;
                case 'C':
                    layer1_queue_key(KEY_TOGGLE_CONTRAST);
                    break;

                /* Navigation keys: route through Layer 1 HAL queue. */
                case VK_LEFT:
                    layer1_queue_key(KEY_PREV);
                    break;
                case VK_RIGHT:
                    layer1_queue_key(KEY_NEXT);
                    break;
                case VK_RETURN:
                    layer1_queue_key(KEY_SELECT);
                    break;
                case VK_ESCAPE:
                    layer1_queue_key(KEY_BACK);
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
    printf("     PROMETHEUS99: LIGHTWEIGHT HMI RUNTIME (C99 STATIC MEMORY)  \n");
    printf("     Team Doomsday: Abhilash L, Adarsh Abhilash, Nikhil Nuguri  \n");
    printf("=================================================================\n");
    printf("[INIT] Initializing 4-Layer Architecture...\n");

    /* Initialize Layers */
    layer0_hardware_init();
    layer1_hal_init();
    layer2_core_init();
    layer3_presentation_init();

    /* Setup Win32 Bitmap Format for 800x480 ARGB Rendering */
    memset(&s_bmi, 0, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = DISPLAY_WIDTH;
    s_bmi.bmiHeader.biHeight = -DISPLAY_HEIGHT; /* Top-down DIB */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;

    /* Register Win32 Window Class */
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = S_WND_CLASS;
    RegisterClassExA(&wc);

    /* Calculate Window Rect for exact 800x480 client area */
    RECT rc = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);

    s_hwnd = CreateWindowExA(
        0,
        S_WND_CLASS,
        "Prometheus99: Lightweight HMI Runtime (C99 Static Memory)",
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

    printf("[INIT] GUI Window Created (800x480 Resolution).\n");

    /* Launch 1000 Hz Sensor ISR Background Thread */
    s_isr_thread = (HANDLE)_beginthreadex(NULL, 0, sensor_isr_thread_proc, NULL, 0, NULL);
    if (!s_isr_thread) {
        fprintf(stderr, "[ERROR] Failed to start 1000 Hz Sensor ISR Thread.\n");
        return 1;
    }

    printf("[INIT] 1000 Hz Sensor Ingestion Thread Active.\n");
    printf("[INIT] 30 Hz UI Presentation Loop Started.\n");
    printf("-----------------------------------------------------------------\n");
    printf(" KEYBOARD SHORTCUTS:\n");
    printf("  [1] Boot Screen      [2] System Dashboard   [3] Binary Diagnostics\n");
    printf("  [4] Alarm Supervisor [5] Settings/Contrast  [6/F] Hot Standby Failover\n");
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

            /* Run 30 Hz Presentation Tick & Dual-Watchdog Check */
            layer3_ui_timer_tick_30hz();

            /* Trigger Redraw on Window */
            InvalidateRect(s_hwnd, NULL, FALSE);
        }

        Sleep(5); /* Yield CPU */
    }

    /* Cleanup */
    if (s_isr_thread) {
        WaitForSingleObject(s_isr_thread, 1000);
        CloseHandle(s_isr_thread);
    }

    /* Properly unregister window class */
    UnregisterClassA(S_WND_CLASS, hInst);

    printf("[SHUTDOWN] Prometheus99 HMI Runtime Terminated Safely.\n");
    return 0;
}
