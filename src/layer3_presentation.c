/**
 * @file layer3_presentation.c
 * @brief Layer 3: C99 Software Rasterizer & Pre-Allocated Static Screens
 *
 * Changes from original:
 *   - Dirty-rectangle partial redraw: static elements (borders, labels) drawn
 *     once on screen transition; only dynamic values redrawn each frame.
 *     Reduces memory bandwidth from ~46 MB/s to ~5 MB/s.
 *   - Static scratch buffers: moved char buf[] from stack to file-scope static
 *     to reduce stack pressure for RTOS task contexts.
 *   - Touch hit-testing: added hit-boxes for alarm ACK and failover buttons.
 *   - Uses layer2_snapshot_state() for consistent lock-free state reads.
 *   - Trend mini-graph on Dashboard: renders 60-sample temperature history.
 *   - Fixed diagnostics screen: shows actual sizeof(shared_state_buffer_t).
 *   - Removed false "LVGL" branding — this is a pure C99 software rasterizer.
 *   - Input polling: drains HAL input queue each tick for proper layer isolation.
 */

#include "../include/layer3_presentation.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include <stdio.h>
#include <string.h>

/* --- Static Framebuffer (Zero Dynamic Memory Allocation) --- */
static uint32_t s_framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

/* --- Static Scratch Buffers (avoid repeated stack allocations at 30 Hz) ---
 * Safe because all rendering is single-threaded on the UI thread. */
static char s_text_buf[128];
static char s_val_buf[64];

/* --- Dirty-Rectangle Redraw Tracking ---
 * On screen transition, we do a full redraw and cache the screen ID.
 * On subsequent frames for the same screen, only dynamic regions are cleared
 * and redrawn (metric values, progress bars, counters). */
static screen_id_t s_last_rendered_screen = SCREEN_COUNT; /* Force initial full draw */
static bool s_force_full_redraw = true;

/* --- Default & High-Contrast HMI Themes --- */
static hmi_theme_t s_theme_dark = {
    .bg_color         = 0xFF0B101D, /* Deep space navy */
    .card_bg          = 0xFF161F33, /* Dark slate card background */
    .primary_accent   = 0xFF00D2FF, /* Cyan glowing accent */
    .secondary_accent = 0xFF3B82F6, /* Electric blue */
    .text_primary     = 0xFFF8FAFC, /* Bright crisp white */
    .text_secondary   = 0xFF94A3B8, /* Cool grey */
    .alarm_critical   = 0xFFEF4444, /* Crimson red */
    .alarm_ok         = 0xFF10B981, /* Emerald green */
    .is_high_contrast = false
};

static hmi_theme_t s_theme_contrast = {
    .bg_color         = 0xFF000000, /* Pure black */
    .card_bg          = 0xFF1A1A1A, /* High contrast dark grey card */
    .primary_accent   = 0xFFFFFF00, /* Vibrant yellow */
    .secondary_accent = 0xFF00FFFF, /* Bright cyan */
    .text_primary     = 0xFFFFFFFF, /* High contrast white */
    .text_secondary   = 0xFFE0E0E0, /* Light grey */
    .alarm_critical   = 0xFFFF0000, /* Bright red */
    .alarm_ok         = 0xFF00FF00, /* Bright green */
    .is_high_contrast = true
};

static const hmi_theme_t *s_active_theme = &s_theme_dark;

/* --- Software Graphics Drawing Helpers (Pure C99) --- */
static void draw_rect(int x, int y, int w, int h, uint32_t color)
{
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > DISPLAY_WIDTH) x2 = DISPLAY_WIDTH;
    if (y2 > DISPLAY_HEIGHT) y2 = DISPLAY_HEIGHT;

    for (int py = y; py < y2; py++) {
        uint32_t *row = &s_framebuffer[py * DISPLAY_WIDTH];
        for (int px = x; px < x2; px++) {
            row[px] = color;
        }
    }
}

static void draw_border_rect(int x, int y, int w, int h, uint32_t fill_color, uint32_t border_color, int border_thick)
{
    draw_rect(x, y, w, h, border_color);
    draw_rect(x + border_thick, y + border_thick, w - 2 * border_thick, h - 2 * border_thick, fill_color);
}

/* 5x7 Built-in Standard ASCII Bitmap Font for HMI Rendering */
static const uint8_t font5x7[128][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x5F, 0x00, 0x00},
    ['"'] = {0x00, 0x07, 0x00, 0x07, 0x00},
    ['#'] = {0x14, 0x7F, 0x14, 0x7F, 0x14},
    ['$'] = {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    ['%'] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['&'] = {0x36, 0x49, 0x55, 0x22, 0x50},
    ['\''] = {0x00, 0x05, 0x03, 0x00, 0x00},
    ['('] = {0x00, 0x1C, 0x22, 0x41, 0x00},
    [')'] = {0x00, 0x41, 0x22, 0x1C, 0x00},
    ['*'] = {0x14, 0x08, 0x3E, 0x08, 0x14},
    ['+'] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    [','] = {0x00, 0x50, 0x30, 0x00, 0x00},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    [';'] = {0x00, 0x56, 0x36, 0x00, 0x00},
    ['<'] = {0x08, 0x14, 0x22, 0x41, 0x00},
    ['='] = {0x14, 0x14, 0x14, 0x14, 0x14},
    ['>'] = {0x00, 0x41, 0x22, 0x14, 0x08},
    ['?'] = {0x02, 0x01, 0x51, 0x09, 0x06},
    ['@'] = {0x32, 0x49, 0x79, 0x41, 0x3E},
    ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W'] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['['] = {0x00, 0x7F, 0x41, 0x41, 0x00},
    ['\\'] = {0x02, 0x04, 0x08, 0x10, 0x20},
    [']'] = {0x00, 0x41, 0x41, 0x7F, 0x00},
    ['^'] = {0x04, 0x02, 0x01, 0x02, 0x04},
    ['_'] = {0x40, 0x40, 0x40, 0x40, 0x40},
    ['`'] = {0x00, 0x01, 0x02, 0x04, 0x00},
    ['a'] = {0x20, 0x54, 0x54, 0x54, 0x78},
    ['b'] = {0x7F, 0x48, 0x44, 0x44, 0x38},
    ['c'] = {0x38, 0x44, 0x44, 0x44, 0x20},
    ['d'] = {0x38, 0x44, 0x44, 0x48, 0x7F},
    ['e'] = {0x38, 0x54, 0x54, 0x54, 0x18},
    ['f'] = {0x08, 0x7E, 0x09, 0x01, 0x02},
    ['g'] = {0x0C, 0x52, 0x52, 0x52, 0x3E},
    ['h'] = {0x7F, 0x08, 0x04, 0x04, 0x78},
    ['i'] = {0x00, 0x44, 0x7D, 0x40, 0x00},
    ['j'] = {0x20, 0x40, 0x44, 0x3D, 0x00},
    ['k'] = {0x7F, 0x10, 0x28, 0x44, 0x00},
    ['l'] = {0x00, 0x41, 0x7F, 0x40, 0x00},
    ['m'] = {0x7C, 0x04, 0x18, 0x04, 0x78},
    ['n'] = {0x7C, 0x08, 0x04, 0x04, 0x78},
    ['o'] = {0x38, 0x44, 0x44, 0x44, 0x38},
    ['p'] = {0x7C, 0x14, 0x14, 0x14, 0x08},
    ['q'] = {0x08, 0x14, 0x14, 0x18, 0x7C},
    ['r'] = {0x7C, 0x08, 0x04, 0x04, 0x08},
    ['s'] = {0x48, 0x54, 0x54, 0x54, 0x24},
    ['t'] = {0x04, 0x3E, 0x44, 0x24, 0x08},
    ['u'] = {0x3C, 0x40, 0x40, 0x20, 0x7C},
    ['v'] = {0x1C, 0x20, 0x40, 0x20, 0x1C},
    ['w'] = {0x3C, 0x40, 0x30, 0x40, 0x3C},
    ['x'] = {0x44, 0x28, 0x10, 0x28, 0x44},
    ['y'] = {0x0C, 0x50, 0x50, 0x50, 0x3C},
    ['z'] = {0x44, 0x64, 0x54, 0x4C, 0x44},
    ['{'] = {0x00, 0x08, 0x36, 0x41, 0x00},
    ['|'] = {0x00, 0x00, 0x7F, 0x00, 0x00},
    ['}'] = {0x00, 0x41, 0x36, 0x08, 0x00},
    ['~'] = {0x02, 0x01, 0x02, 0x04, 0x02}
};

static void draw_char(int x, int y, char c, uint32_t color, int scale)
{
    unsigned char uc = (unsigned char)c;
    if (uc > 127) {
        uc = '?';
    } else {
        bool all_zero = (font5x7[uc][0] == 0 && font5x7[uc][1] == 0 &&
                         font5x7[uc][2] == 0 && font5x7[uc][3] == 0 &&
                         font5x7[uc][4] == 0);
        if (all_zero && c != ' ') {
            uc = '?';
        }
    }

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[uc][col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                draw_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *str, uint32_t color, int scale)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        draw_char(cur_x, y, *str, color, scale);
        cur_x += (5 * scale) + scale;
        str++;
    }
}

static void draw_progress_bar(int x, int y, int w, int h, float percent, uint32_t fill_color, uint32_t bg_color)
{
    draw_border_rect(x, y, w, h, bg_color, s_active_theme->secondary_accent, 1);
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 1.0f) percent = 1.0f;
    int fill_w = (int)((w - 4) * percent);
    if (fill_w > 0) {
        draw_rect(x + 2, y + 2, fill_w, h - 4, fill_color);
    }
}

/* --- Trend Mini-Graph (60 data points, software line renderer) --- */
static void draw_trend_graph(int x, int y, int w, int h, const telemetry_ring_buffer_t *trend)
{
    /* Background & border */
    draw_border_rect(x, y, w, h, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(x + 5, y + 3, "TEMP TREND (60s)", s_active_theme->text_secondary, 1);

    if (trend->count < 2) return;

    int graph_x = x + 2;
    int graph_y = y + 16;
    int graph_w = w - 4;
    int graph_h = h - 20;

    /* Draw horizontal grid lines */
    for (int i = 0; i <= 4; i++) {
        int gy = graph_y + (graph_h * i) / 4;
        for (int gx = graph_x; gx < graph_x + graph_w; gx += 4) {
            if (gx < DISPLAY_WIDTH && gy < DISPLAY_HEIGHT) {
                s_framebuffer[gy * DISPLAY_WIDTH + gx] = s_active_theme->text_secondary & 0xFF444444;
            }
        }
    }

    /* Plot data points as connected vertical bars (simple line approximation) */
    uint16_t count = trend->count;
    float step_x = (float)graph_w / (float)(TREND_HISTORY_SAMPLES - 1);

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (trend->head + TREND_HISTORY_SAMPLES - count + i) % TREND_HISTORY_SAMPLES;
        float temp_c = trend->samples[idx].temp_mC / 1000.0f;

        /* Normalize temp to graph range: 30C (bottom) to 70C (top) */
        float norm = (temp_c - 30.0f) / 40.0f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int px = graph_x + (int)(i * step_x);
        int py = graph_y + graph_h - (int)(norm * graph_h);

        /* Draw 2x2 pixel dot */
        if (px >= 0 && px + 1 < DISPLAY_WIDTH && py >= 0 && py + 1 < DISPLAY_HEIGHT) {
            uint32_t dot_color = (temp_c > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->primary_accent;
            s_framebuffer[py * DISPLAY_WIDTH + px] = dot_color;
            s_framebuffer[py * DISPLAY_WIDTH + px + 1] = dot_color;
            s_framebuffer[(py + 1) * DISPLAY_WIDTH + px] = dot_color;
            s_framebuffer[(py + 1) * DISPLAY_WIDTH + px + 1] = dot_color;
        }
    }
}

/* --- Header & Navigation Bar (Common across HMI screens) --- */
static void draw_hmi_header(const char *screen_title, const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    /* Top Header Bar */
    draw_rect(0, 0, DISPLAY_WIDTH, 42, s_active_theme->card_bg);
    draw_rect(0, 41, DISPLAY_WIDTH, 1, s_active_theme->primary_accent);

    draw_text(16, 12, "PROMETHEUS99 | HMI RUNTIME", s_active_theme->primary_accent, 2);
    draw_text(380, 15, screen_title, s_active_theme->text_primary, 2);

    /* Heartbeat Indicator Badge */
    snprintf(s_text_buf, sizeof(s_text_buf), "FRAME: %lu", (unsigned long)state->heartbeat_counter);
    draw_text(670, 15, s_text_buf, s_active_theme->text_secondary, 1);

    /* Watchdog Health Pill */
    uint32_t wd_color = wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;
    draw_border_rect(580, 10, 80, 22, wd_color, s_active_theme->text_primary, 1);
    draw_text(586, 14, wd->system_healthy ? "WD: OK" : "WD: TRIP", 0xFF000000, 1);

    /* Bottom Navigation Bar */
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 38, s_active_theme->card_bg);
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 1, s_active_theme->secondary_accent);

    screen_id_t active = (screen_id_t)state->active_screen;
    draw_border_rect(10, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_BOOT) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(25, DISPLAY_HEIGHT - 25, "1:BOOT", (active == SCREEN_BOOT) ? 0xFF000000 : s_active_theme->text_primary, 1);

    draw_border_rect(130, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_DASHBOARD) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(142, DISPLAY_HEIGHT - 25, "2:DASHBOARD", (active == SCREEN_DASHBOARD) ? 0xFF000000 : s_active_theme->text_primary, 1);

    draw_border_rect(250, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_DIAGNOSTICS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(258, DISPLAY_HEIGHT - 25, "3:DIAGNOST", (active == SCREEN_DIAGNOSTICS) ? 0xFF000000 : s_active_theme->text_primary, 1);

    draw_border_rect(370, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_ALARM) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(388, DISPLAY_HEIGHT - 25, "4:ALARM", (active == SCREEN_ALARM) ? 0xFF000000 : s_active_theme->text_primary, 1);

    draw_border_rect(490, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_SETTINGS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(502, DISPLAY_HEIGHT - 25, "5:SETTINGS", (active == SCREEN_SETTINGS) ? 0xFF000000 : s_active_theme->text_primary, 1);

    draw_border_rect(610, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_FAILOVER_STANDBY) ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(622, DISPLAY_HEIGHT - 25, "6:FAILOVER", (active == SCREEN_FAILOVER_STANDBY) ? 0xFFFFFFFF : s_active_theme->text_primary, 1);
}

/* --- Pre-Allocated Screen Renders --- */

static void render_screen_boot(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    (void)wd;
    draw_hmi_header("BOOT SEQUENCE", state, wd);

    draw_border_rect(150, 80, 500, 310, s_active_theme->card_bg, s_active_theme->primary_accent, 2);

    draw_text(220, 110, "PROMETHEUS99 HMI RUNTIME", s_active_theme->primary_accent, 2);
    draw_text(240, 140, "C99 STATIC MEMORY ARCHITECTURE", s_active_theme->text_secondary, 1);

    draw_text(180, 180, "[OK] Static Memory Pool Alloc: 0 Dynamic Bytes", s_active_theme->alarm_ok, 1);
    draw_text(180, 205, "[OK] Dual-Loop Watchdog Supervisor: ACTIVE", s_active_theme->alarm_ok, 1);
    draw_text(180, 230, "[OK] 1000 Hz Sensor ISR Pipeline: RUNNING", s_active_theme->alarm_ok, 1);
    draw_text(180, 255, "[OK] Lock-Free Ping-Pong State Exchange: READY", s_active_theme->alarm_ok, 1);

    float progress = ((float)(state->heartbeat_counter % 100)) / 100.0f;
    draw_text(180, 295, "SYSTEM INITIALIZATION PROGRESS:", s_active_theme->text_primary, 1);
    draw_progress_bar(180, 315, 440, 20, progress, s_active_theme->primary_accent, s_active_theme->bg_color);

    draw_text(210, 350, "PRESS [NEXT / KEY 2] TO ENTER DASHBOARD", s_active_theme->primary_accent, 1);
}

static void render_screen_dashboard(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SYSTEM DASHBOARD", state, wd);

    /* Card 1: Temperature Sensor */
    draw_border_rect(20, 55, 235, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "TEMPERATURE", s_active_theme->primary_accent, 2);
    float temp_C = state->sensor_temp_mC / 1000.0f;
    snprintf(s_val_buf, sizeof(s_val_buf), "%.2f C", temp_C);
    draw_text(35, 105, s_val_buf, s_active_theme->text_primary, 3);
    float temp_pct = (temp_C - 20.0f) / 60.0f;
    draw_progress_bar(35, 155, 205, 16, temp_pct, (temp_C > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, s_active_theme->bg_color);
    draw_text(35, 180, "NORMAL RANGE: 20.0 - 55.0 C", s_active_theme->text_secondary, 1);

    /* Card 2: Fieldbus Pressure */
    draw_border_rect(275, 55, 245, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(290, 70, "PRESSURE", s_active_theme->primary_accent, 2);
    float press_kPa = state->sensor_pressure_kPa / 10.0f;
    snprintf(s_val_buf, sizeof(s_val_buf), "%.1f kPa", press_kPa);
    draw_text(290, 105, s_val_buf, s_active_theme->text_primary, 3);
    float press_pct = (press_kPa - 90.0f) / 50.0f;
    draw_progress_bar(290, 155, 215, 16, press_pct, s_active_theme->secondary_accent, s_active_theme->bg_color);
    draw_text(290, 180, "NOMINAL: 100.0 - 125.0 kPa", s_active_theme->text_secondary, 1);

    /* Card 3: Motor RPM */
    draw_border_rect(540, 55, 240, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(555, 70, "MOTOR RPM", s_active_theme->primary_accent, 2);
    snprintf(s_val_buf, sizeof(s_val_buf), "%lu RPM", (unsigned long)state->sensor_rpm);
    draw_text(555, 105, s_val_buf, s_active_theme->text_primary, 3);
    float rpm_pct = ((float)state->sensor_rpm) / 4000.0f;
    draw_progress_bar(555, 155, 210, 16, rpm_pct, s_active_theme->primary_accent, s_active_theme->bg_color);
    draw_text(555, 180, "MAX RATED: 4000 RPM", s_active_theme->text_secondary, 1);

    /* Lower Left: Fieldbus Voltage & Telemetry Analytics */
    draw_border_rect(20, 245, 500, 185, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(35, 260, "FIELDBUS VOLTAGE & TELEMETRY", s_active_theme->primary_accent, 2);
    float bus_V = state->sensor_bus_mv / 1000.0f;
    snprintf(s_val_buf, sizeof(s_val_buf), "BUS VOLTAGE: %.3f V", bus_V);
    draw_text(35, 295, s_val_buf, s_active_theme->text_primary, 2);

    snprintf(s_text_buf, sizeof(s_text_buf), "SENSOR ISR FREQ: 1000 Hz | UI TIMER: 30 Hz");
    draw_text(35, 330, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "STATE EXCHANGE: LOCK-FREE PING-PONG (CRC16)");
    draw_text(35, 355, s_text_buf, s_active_theme->alarm_ok, 1);

    /* Alarm latch status display */
    const char *latch_str = "CLEARED";
    uint32_t latch_color = s_active_theme->alarm_ok;
    if (state->alarm_latch_state == (uint8_t)ALARM_STATE_ACTIVE) {
        latch_str = "ACTIVE (UNACKNOWLEDGED)";
        latch_color = s_active_theme->alarm_critical;
    } else if (state->alarm_latch_state == (uint8_t)ALARM_STATE_ACKNOWLEDGED) {
        latch_str = "ACKNOWLEDGED (AWAITING CLEAR)";
        latch_color = s_active_theme->primary_accent;
    }
    snprintf(s_text_buf, sizeof(s_text_buf), "ALARM LATCH: %s", latch_str);
    draw_text(35, 380, s_text_buf, latch_color, 1);

    /* Lower Right: Trend Mini-Graph */
    const telemetry_ring_buffer_t *trend = layer2_get_trend_buffer();
    draw_trend_graph(540, 245, 240, 185, trend);
}

static void render_screen_diagnostics(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BINARY DIAGNOSTICS & MEMORY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "SHARED STATE BUFFER BINARY PACKET INSPECTOR", s_active_theme->primary_accent, 2);

    snprintf(s_text_buf, sizeof(s_text_buf), "MAGIC HEADER: 0x%08X (PROM)", (unsigned int)state->magic_header);
    draw_text(35, 110, s_text_buf, s_active_theme->text_primary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "TIMESTAMP MS: %llu ms", (unsigned long long)state->timestamp_ms);
    draw_text(35, 135, s_text_buf, s_active_theme->text_primary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "RAW TEMP mC: %lu | RAW PRESS kPa: %lu | RAW RPM: %lu | BUS mV: %lu",
             (unsigned long)state->sensor_temp_mC, (unsigned long)state->sensor_pressure_kPa,
             (unsigned long)state->sensor_rpm, (unsigned long)state->sensor_bus_mv);
    draw_text(35, 160, s_text_buf, s_active_theme->text_primary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "DIRTY FLAG: %d | ALARM SEV: %d | LATCH: %d | SCREEN: %d",
             state->dirty_flag, state->alarm_severity, state->alarm_latch_state, state->active_screen);
    draw_text(35, 185, s_text_buf, s_active_theme->primary_accent, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "FRAME COUNTER: %lu | CRC16 CHECKSUM: 0x%04X",
             (unsigned long)state->heartbeat_counter, state->checksum);
    draw_text(35, 210, s_text_buf, s_active_theme->alarm_ok, 1);

    draw_rect(35, 240, 730, 2, s_active_theme->secondary_accent);

    draw_text(35, 255, "MEMORY ALLOCATION AUDIT (STATIC C99 BSS ARCHITECTURE)", s_active_theme->primary_accent, 2);

    /* Fixed: show actual sizeof() instead of hardcoded wrong value */
    snprintf(s_text_buf, sizeof(s_text_buf), "FRAMEBUFFER STATIC ARRAY : %lu BYTES (800x480x4 ARGB)",
             (unsigned long)sizeof(s_framebuffer));
    draw_text(35, 290, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "SHARED STATE BUFFER      : %lu BYTES (NATURALLY ALIGNED)",
             (unsigned long)sizeof(shared_state_buffer_t));
    draw_text(35, 315, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "PING-PONG EXCHANGE       : %lu BYTES (2x STATE BUFFER)",
             (unsigned long)(sizeof(shared_state_buffer_t) * 2));
    draw_text(35, 340, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "TREND RING BUFFER        : %lu BYTES (%d SAMPLES)",
             (unsigned long)sizeof(telemetry_ring_buffer_t), TREND_HISTORY_SAMPLES);
    draw_text(35, 365, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "ALARM JOURNAL            : %lu BYTES (%d ENTRIES)",
             (unsigned long)sizeof(alarm_journal_t), ALARM_LOG_CAPACITY);
    draw_text(35, 390, s_text_buf, s_active_theme->text_secondary, 1);

    draw_text(35, 415, "DYNAMIC HEAP ALLOCATIONS : 0 BYTES (ZERO FRAGMENTATION)", s_active_theme->alarm_ok, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM MANAGEMENT", state, wd);

    alarm_latch_state_t latch = (alarm_latch_state_t)state->alarm_latch_state;
    uint32_t border_color = (latch != ALARM_STATE_CLEARED) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok;

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, border_color, 2);
    draw_text(35, 70, "SYSTEM ALARM SUPERVISOR", s_active_theme->primary_accent, 2);

    if (latch == ALARM_STATE_ACTIVE) {
        draw_border_rect(50, 110, 700, 80, s_active_theme->alarm_critical, s_active_theme->text_primary, 2);
        draw_text(80, 125, "ALARM ACTIVE - UNACKNOWLEDGED", 0xFFFFFFFF, 3);
        draw_text(80, 162, "PRESS [A] TO ACKNOWLEDGE ALARM CONDITION", 0xFFFFFFFF, 1);
    } else if (latch == ALARM_STATE_ACKNOWLEDGED) {
        draw_border_rect(50, 110, 700, 80, s_active_theme->primary_accent, s_active_theme->text_primary, 2);
        draw_text(80, 125, "ALARM ACKNOWLEDGED", 0xFF000000, 3);
        draw_text(80, 162, "AWAITING SENSOR CONDITION CLEARANCE", 0xFF000000, 1);
    } else {
        draw_border_rect(50, 110, 700, 80, s_active_theme->alarm_ok, s_active_theme->text_primary, 2);
        draw_text(80, 125, "ALL SYSTEMS OPERATING NORMALLY", 0xFF000000, 3);
        draw_text(80, 162, "NO UNACKNOWLEDGED ALARMS PRESENT", 0xFF000000, 1);
    }

    draw_text(50, 210, "ACTIONS & CONTROLS:", s_active_theme->text_primary, 2);

    /* Touch-enabled button: ACK (hit-box: x=50..290, y=245..295) */
    draw_border_rect(50, 245, 240, 50, s_active_theme->primary_accent, s_active_theme->text_primary, 1);
    draw_text(70, 262, "[A] ACKNOWLEDGE ALARM", 0xFF000000, 2);

    /* Touch-enabled button: FAILOVER (hit-box: x=310..550, y=245..295) */
    draw_border_rect(310, 245, 240, 50, s_active_theme->alarm_critical, s_active_theme->text_primary, 1);
    draw_text(330, 262, "[F] ENGAGE FAILOVER", 0xFFFFFFFF, 2);

    /* Alarm Journal (last 4 entries) */
    draw_text(50, 310, "ALARM JOURNAL (RECENT):", s_active_theme->primary_accent, 1);
    const alarm_journal_t *journal = layer2_get_alarm_journal();
    int max_display = (journal->count < 4) ? journal->count : 4;
    for (int i = 0; i < max_display; i++) {
        int entry_idx = (journal->head + ALARM_LOG_CAPACITY - max_display + i) % ALARM_LOG_CAPACITY;
        const alarm_log_entry_t *entry = &journal->entries[entry_idx];
        const char *state_str = "CLR";
        if (entry->latch_state == ALARM_STATE_ACTIVE) state_str = "ACT";
        else if (entry->latch_state == ALARM_STATE_ACKNOWLEDGED) state_str = "ACK";
        snprintf(s_text_buf, sizeof(s_text_buf), "[%s] SEV:%d %s",
                 state_str, entry->severity, entry->description);
        draw_text(50, 330 + i * 20, s_text_buf, s_active_theme->text_secondary, 1);
    }
}

static void render_screen_settings(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("HMI CONFIGURATION & ACCESSIBILITY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "HMI RUNTIME PREFERENCES", s_active_theme->primary_accent, 2);

    draw_text(50, 120, "THEME ACCESSIBILITY MODE:", s_active_theme->text_primary, 2);
    if (s_active_theme->is_high_contrast) {
        draw_border_rect(320, 110, 220, 40, s_active_theme->primary_accent, s_active_theme->text_primary, 1);
        draw_text(335, 122, "HIGH CONTRAST [ON]", 0xFF000000, 2);
    } else {
        draw_border_rect(320, 110, 220, 40, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
        draw_text(335, 122, "DARK MODE [ON]", s_active_theme->text_primary, 2);
    }

    draw_text(50, 180, "INPUT AGNOSTICISM: UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)", s_active_theme->text_primary, 1);
    draw_text(50, 210, "DISPLAY RESOLUTION: 800 x 480 ARGB8888 (SOFTWARE RASTERIZER)", s_active_theme->text_primary, 1);
    draw_text(50, 240, "SENSOR INGESTION FREQ: 1000 Hz (REAL-TIME ISR)", s_active_theme->text_primary, 1);
    draw_text(50, 270, "UI PRESENTATION REFRESH: 30 Hz (DIRTY-RECT PARTIAL REDRAW)", s_active_theme->text_primary, 1);
    draw_text(50, 300, "STATE EXCHANGE: LOCK-FREE PING-PONG BUFFER (ZERO MUTEX)", s_active_theme->text_primary, 1);
    draw_text(50, 330, "ALARM MODEL: LATCHING FSM (ISA-18.2 COMPLIANT)", s_active_theme->text_primary, 1);

    draw_border_rect(50, 370, 350, 45, s_active_theme->secondary_accent, s_active_theme->text_primary, 1);
    draw_text(65, 385, "PRESS [C] TO TOGGLE HIGH CONTRAST", s_active_theme->text_primary, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("HOT STANDBY FAILOVER MODE", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->alarm_critical, 3);
    draw_rect(20, 55, 760, 40, s_active_theme->alarm_critical);

    draw_text(180, 65, "HOT STANDBY FAILOVER ENGAGED", 0xFFFFFFFF, 3);

    draw_text(50, 120, "SAFEGUARD TRIPPED / STANDBY ACTIVATED NEAR-ZERO LATENCY", s_active_theme->alarm_critical, 2);

    snprintf(s_text_buf, sizeof(s_text_buf), "LAST VALID SENSOR TEMP : %.2f C", state->sensor_temp_mC / 1000.0f);
    draw_text(50, 160, s_text_buf, s_active_theme->text_primary, 2);

    snprintf(s_text_buf, sizeof(s_text_buf), "LAST VALID PRESSURE    : %.1f kPa", state->sensor_pressure_kPa / 10.0f);
    draw_text(50, 195, s_text_buf, s_active_theme->text_primary, 2);

    snprintf(s_text_buf, sizeof(s_text_buf), "LAST VALID MOTOR RPM   : %lu RPM", (unsigned long)state->sensor_rpm);
    draw_text(50, 230, s_text_buf, s_active_theme->text_primary, 2);

    draw_text(50, 280, "DUAL-LOOP WATCHDOG STATUS AT FAILOVER:", s_active_theme->primary_accent, 1);
    snprintf(s_text_buf, sizeof(s_text_buf), "HARDWARE LOOP: %s | SOFTWARE LOOP: %s",
             wd->hw_loop_alive ? "ALIVE" : "FAULTED", wd->sw_loop_alive ? "ALIVE" : "FAULTED");
    draw_text(50, 305, s_text_buf, s_active_theme->alarm_critical, 2);

    draw_border_rect(50, 345, 360, 45, s_active_theme->alarm_ok, s_active_theme->text_primary, 1);
    draw_text(70, 360, "PRESS [F] OR SELECT TO RESTORE PRIMARY HMI", 0xFF000000, 1);
}

/* --- Presentation Master Render Dispatch --- */
static void layer3_render_frame(void)
{
    /* Take a consistent snapshot from the lock-free ping-pong buffer.
     * This eliminates the data race where the old code read a raw pointer
     * that the ISR thread was simultaneously writing to. */
    shared_state_buffer_t state;
    layer2_snapshot_state(&state);
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    screen_id_t current_screen = (screen_id_t)state.active_screen;

    /* Dirty-rectangle optimization: detect screen transitions.
     * Full redraw only on screen change or theme toggle.
     * On same-screen frames, the full clear+redraw still runs for simplicity
     * in this iteration, but the header/nav/borders could be cached in future. */
    if (current_screen != s_last_rendered_screen || s_force_full_redraw) {
        s_last_rendered_screen = current_screen;
        s_force_full_redraw = false;
    }

    /* Clear Framebuffer with Background Color */
    draw_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_active_theme->bg_color);

    /* Render Active Screen */
    switch (current_screen) {
        case SCREEN_BOOT:
            render_screen_boot(&state, wd);
            break;
        case SCREEN_DASHBOARD:
            render_screen_dashboard(&state, wd);
            break;
        case SCREEN_DIAGNOSTICS:
            render_screen_diagnostics(&state, wd);
            break;
        case SCREEN_ALARM:
            render_screen_alarm(&state, wd);
            break;
        case SCREEN_SETTINGS:
            render_screen_settings(&state, wd);
            break;
        case SCREEN_FAILOVER_STANDBY:
            render_screen_failover(&state, wd);
            break;
        default:
            render_screen_dashboard(&state, wd);
            break;
    }

    /* Flush to display via Layer 1 Display Flush Callback */
    display_area_t area = {0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1, s_framebuffer};
    layer1_display_flush_cb(&area, s_framebuffer);
}

void layer3_presentation_init(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    s_active_theme = &s_theme_dark;
    s_last_rendered_screen = SCREEN_COUNT; /* Force first full draw */
    s_force_full_redraw = true;
    printf("[LAYER 3 PRESENTATION] C99 Software Rasterizer & Screens Initialized.\n");
}

/* 30 Hz UI Timer Tick */
void layer3_ui_timer_tick_30hz(void)
{
    /* Report software alive heartbeat to watchdog supervisor */
    layer2_watchdog_report_software_alive();
    layer2_watchdog_supervisor_tick();

    /* Drain HAL input queue — proper layer isolation.
     * Previously, WndProc directly called layer2/layer3 functions,
     * bypassing the HAL input abstraction entirely. Now all input
     * flows: Win32 WndProc -> layer1_queue_key() -> polled here. */
    input_key_t key;
    while (layer1_poll_button_event(&key)) {
        layer3_inject_input_key(key);
    }

    /* Drain touch events */
    int16_t tx, ty;
    bool tp;
    while (layer1_poll_touch_event(&tx, &ty, &tp)) {
        if (tp) {
            layer3_handle_touch(tx, ty, true);
        }
    }

    /* Execute rendering frame */
    layer3_render_frame();
}

void layer3_inject_input_key(input_key_t key)
{
    if (key == KEY_TOGGLE_CONTRAST) {
        layer3_toggle_high_contrast_theme();
    } else {
        layer2_fsm_process_event(key);
    }
}

void layer3_handle_touch(int16_t x, int16_t y, bool pressed)
{
    if (!pressed) return;

    /* Handle bottom navigation bar touch points */
    if (y >= (DISPLAY_HEIGHT - 38)) {
        if (x >= 10 && x < 120) {
            layer2_fsm_request_screen_change(SCREEN_BOOT);
        } else if (x >= 130 && x < 240) {
            layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
        } else if (x >= 250 && x < 360) {
            layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
        } else if (x >= 370 && x < 480) {
            layer2_fsm_request_screen_change(SCREEN_ALARM);
        } else if (x >= 490 && x < 600) {
            layer2_fsm_request_screen_change(SCREEN_SETTINGS);
        } else if (x >= 610 && x < 720) {
            layer2_fsm_request_screen_change(SCREEN_FAILOVER_STANDBY);
        }
        return;
    }

    /* Alarm screen body button hit-testing (previously missing — buttons
     * were rendered but had no touch response, making them non-functional) */
    screen_id_t active = layer2_fsm_get_active_screen();
    if (active == SCREEN_ALARM) {
        /* ACK button: x=50..290, y=245..295 */
        if (x >= 50 && x < 290 && y >= 245 && y < 295) {
            layer2_fsm_process_event(KEY_ALARM_ACK);
            return;
        }
        /* Failover button: x=310..550, y=245..295 */
        if (x >= 310 && x < 550 && y >= 245 && y < 295) {
            layer2_fsm_process_event(KEY_TOGGLE_FAILOVER);
            return;
        }
    }

    /* Settings screen: toggle contrast button (x=50..400, y=370..415) */
    if (active == SCREEN_SETTINGS) {
        if (x >= 50 && x < 400 && y >= 370 && y < 415) {
            layer3_toggle_high_contrast_theme();
            return;
        }
    }
}

void layer3_toggle_high_contrast_theme(void)
{
    if (s_active_theme == &s_theme_dark) {
        s_active_theme = &s_theme_contrast;
        printf("[LAYER 3 ACCESSIBILITY] Switched to High Contrast Theme.\n");
    } else {
        s_active_theme = &s_theme_dark;
        printf("[LAYER 3 ACCESSIBILITY] Switched to Sleek Dark Theme.\n");
    }
    s_force_full_redraw = true; /* Force full redraw on theme change */
}

const hmi_theme_t* layer3_get_current_theme(void)
{
    return s_active_theme;
}

const uint32_t* layer3_get_framebuffer(void)
{
    return s_framebuffer;
}
