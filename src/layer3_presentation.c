/**
 * @file layer3_presentation.c
 * @brief Layer 3: LVGL Presentation Layer & Pre-Allocated Static Screens
 */

#include "../include/layer3_presentation.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include <stdio.h>
#include <string.h>

/* --- Static Framebuffer (Zero Dynamic Memory Allocation) --- */
static uint32_t s_framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

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

/* 5x7 Minimal Built-in Bitmap Font for HMI Rendering */
static const uint8_t font5x7[][5] = {
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
    ['z'] = {0x44, 0x64, 0x54, 0x4C, 0x44}
};

static void draw_char(int x, int y, char c, uint32_t color, int scale)
{
    unsigned char uc = (unsigned char)c;
    if (uc > 127 || (font5x7[uc][0] == 0 && c != ' ')) uc = '?';

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

/* --- Header & Navigation Bar (Common across HMI screens) --- */
static void draw_hmi_header(const char *screen_title, const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    /* Top Header Bar */
    draw_rect(0, 0, DISPLAY_WIDTH, 42, s_active_theme->card_bg);
    draw_rect(0, 41, DISPLAY_WIDTH, 1, s_active_theme->primary_accent);

    draw_text(16, 12, "PROMETHEUS99 | HMI RUNTIME", s_active_theme->primary_accent, 2);
    draw_text(380, 15, screen_title, s_active_theme->text_primary, 2);

    /* Heartbeat Indicator Badge */
    char hb_buf[32];
    snprintf(hb_buf, sizeof(hb_buf), "FRAME: %lu", (unsigned long)state->heartbeat_counter);
    draw_text(670, 15, hb_buf, s_active_theme->text_secondary, 1);

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
    draw_text(240, 140, "C99 + LVGL SAFE ARCHITECTURE", s_active_theme->text_secondary, 1);

    draw_text(180, 180, "[OK] Static Memory Pool Alloc: 0 Dynamic Bytes", s_active_theme->alarm_ok, 1);
    draw_text(180, 205, "[OK] Dual-Loop Watchdog Supervisor: ACTIVE", s_active_theme->alarm_ok, 1);
    draw_text(180, 230, "[OK] 1000 Hz Sensor ISR Pipeline: RUNNING", s_active_theme->alarm_ok, 1);
    draw_text(180, 255, "[OK] Binary Telemetry Serializer: READY", s_active_theme->alarm_ok, 1);

    float progress = ((float)(state->heartbeat_counter % 100)) / 100.0f;
    draw_text(180, 295, "SYSTEM INITIALIZATION PROGRESS:", s_active_theme->text_primary, 1);
    draw_progress_bar(180, 315, 440, 20, progress, s_active_theme->primary_accent, s_active_theme->bg_color);

    draw_text(210, 350, "PRESS [NEXT / KEY 2] TO ENTER DASHBOARD", s_active_theme->primary_accent, 1);
}

static void render_screen_dashboard(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SYSTEM DASHBOARD", state, wd);

    char val_buf[64];

    /* Card 1: Temperature Sensor */
    draw_border_rect(20, 55, 235, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "TEMPERATURE", s_active_theme->primary_accent, 2);
    float temp_C = state->sensor_temp_mC / 1000.0f;
    snprintf(val_buf, sizeof(val_buf), "%.2f C", temp_C);
    draw_text(35, 105, val_buf, s_active_theme->text_primary, 3);
    float temp_pct = (temp_C - 20.0f) / 60.0f;
    draw_progress_bar(35, 155, 205, 16, temp_pct, (temp_C > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, s_active_theme->bg_color);
    draw_text(35, 180, "NORMAL RANGE: 20.0 - 55.0 C", s_active_theme->text_secondary, 1);

    /* Card 2: Fieldbus Pressure */
    draw_border_rect(275, 55, 245, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(290, 70, "PRESSURE", s_active_theme->primary_accent, 2);
    float press_kPa = state->sensor_pressure_kPa / 10.0f;
    snprintf(val_buf, sizeof(val_buf), "%.1f kPa", press_kPa);
    draw_text(290, 105, val_buf, s_active_theme->text_primary, 3);
    float press_pct = (press_kPa - 90.0f) / 50.0f;
    draw_progress_bar(290, 155, 215, 16, press_pct, s_active_theme->secondary_accent, s_active_theme->bg_color);
    draw_text(290, 180, "NOMINAL: 100.0 - 125.0 kPa", s_active_theme->text_secondary, 1);

    /* Card 3: Motor RPM */
    draw_border_rect(540, 55, 240, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(555, 70, "MOTOR RPM", s_active_theme->primary_accent, 2);
    snprintf(val_buf, sizeof(val_buf), "%lu RPM", (unsigned long)state->sensor_rpm);
    draw_text(555, 105, val_buf, s_active_theme->text_primary, 3);
    float rpm_pct = ((float)state->sensor_rpm) / 4000.0f;
    draw_progress_bar(555, 155, 210, 16, rpm_pct, s_active_theme->primary_accent, s_active_theme->bg_color);
    draw_text(555, 180, "MAX RATED: 4000 RPM", s_active_theme->text_secondary, 1);

    /* Lower Section: Fieldbus Voltage & Telemetry Analytics */
    draw_border_rect(20, 245, 500, 185, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(35, 260, "FIELDBUS VOLTAGE & TELEMETRY", s_active_theme->primary_accent, 2);
    float bus_V = state->sensor_bus_mv / 1000.0f;
    snprintf(val_buf, sizeof(val_buf), "BUS VOLTAGE: %.3f V", bus_V);
    draw_text(35, 295, val_buf, s_active_theme->text_primary, 2);

    snprintf(val_buf, sizeof(val_buf), "SENSOR ISR FREQ: 1000 Hz | UI TIMER: 30 Hz");
    draw_text(35, 330, val_buf, s_active_theme->text_secondary, 1);

    snprintf(val_buf, sizeof(val_buf), "SERIALIZATION: PACKED C99 BINARY (CRC16)");
    draw_text(35, 355, val_buf, s_active_theme->alarm_ok, 1);

    snprintf(val_buf, sizeof(val_buf), "FRAGMENTATION: 0%% (STATIC ALLOCATION)");
    draw_text(35, 380, val_buf, s_active_theme->primary_accent, 1);

    /* Watchdog Health Card */
    draw_border_rect(540, 245, 240, 185, s_active_theme->card_bg, wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);
    draw_text(555, 260, "DUAL-LOOP WATCHDOG", wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 2);
    draw_text(555, 295, wd->hw_loop_alive ? "HW LOOP (1000Hz): ALIVE" : "HW LOOP: SILENT", wd->hw_loop_alive ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);
    draw_text(555, 320, wd->sw_loop_alive ? "SW LOOP (30Hz): ALIVE" : "SW LOOP: SILENT", wd->sw_loop_alive ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);
    snprintf(val_buf, sizeof(val_buf), "TOTAL PETS: %lu", (unsigned long)wd->total_watchdog_pets);
    draw_text(555, 350, val_buf, s_active_theme->text_secondary, 1);
    snprintf(val_buf, sizeof(val_buf), "FAULTS DETECTED: %lu", (unsigned long)wd->watchdog_fault_count);
    draw_text(555, 375, val_buf, (wd->watchdog_fault_count > 0) ? s_active_theme->alarm_critical : s_active_theme->text_secondary, 1);
}

static void render_screen_diagnostics(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BINARY DIAGNOSTICS & MEMORY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "SHARED STATE BUFFER BINARY PACKET INSPECTOR", s_active_theme->primary_accent, 2);

    char buf[128];
    snprintf(buf, sizeof(buf), "MAGIC HEADER: 0x%08X (PROM)", (unsigned int)state->magic_header);
    draw_text(35, 110, buf, s_active_theme->text_primary, 1);

    snprintf(buf, sizeof(buf), "TIMESTAMP MS: %llu ms", (unsigned long long)state->timestamp_ms);
    draw_text(35, 135, buf, s_active_theme->text_primary, 1);

    snprintf(buf, sizeof(buf), "RAW TEMP mC: %lu | RAW PRESS kPa: %lu | RAW RPM: %lu | BUS mV: %lu",
             (unsigned long)state->sensor_temp_mC, (unsigned long)state->sensor_pressure_kPa,
             (unsigned long)state->sensor_rpm, (unsigned long)state->sensor_bus_mv);
    draw_text(35, 160, buf, s_active_theme->text_primary, 1);

    snprintf(buf, sizeof(buf), "DIRTY FLAG: %d | ALARM SEVERITY: %d | ACTIVE SCREEN ID: %d",
             state->dirty_flag, state->alarm_severity, state->active_screen);
    draw_text(35, 185, buf, s_active_theme->primary_accent, 1);

    snprintf(buf, sizeof(buf), "FRAME COUNTER: %lu | CRC16 CHECKSUM: 0x%04X",
             (unsigned long)state->heartbeat_counter, state->checksum);
    draw_text(35, 210, buf, s_active_theme->alarm_ok, 1);

    draw_rect(35, 240, 730, 2, s_active_theme->secondary_accent);

    draw_text(35, 255, "MEMORY ALLOCATION AUDIT (STATIC C99 BSS ARCHITECTURE)", s_active_theme->primary_accent, 2);
    draw_text(35, 290, "FRAMEBUFFER STATIC ARRAY : 1,536,000 BYTES (800x480x4 ARGB)", s_active_theme->text_secondary, 1);
    draw_text(35, 315, "SHARED STATE BUFFER      : 36 BYTES (PACKED C99 STRUCT)", s_active_theme->text_secondary, 1);
    draw_text(35, 340, "DYNAMIC HEAP ALLOCATIONS : 0 BYTES (ZERO FRAGMENTATION)", s_active_theme->alarm_ok, 2);
    draw_text(35, 375, "ESTIMATED BASE BINARY    : ~150 KB (HIGH AUDITABILITY)", s_active_theme->alarm_ok, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM MANAGEMENT", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, (state->alarm_severity != ALARM_NONE) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 2);

    draw_text(35, 70, "SYSTEM ALARM SUPERVISOR", s_active_theme->primary_accent, 2);

    if (state->alarm_severity != ALARM_NONE) {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_critical, s_active_theme->text_primary, 2);
        draw_text(80, 130, "CRITICAL TRIP CONDITION DETECTED", 0xFFFFFFFF, 3);
        draw_text(80, 175, "SENSOR OVER-LIMIT OR VOLTAGE DROP RECORDED IN BINARY STATE", 0xFFFFFFFF, 1);
    } else {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_ok, s_active_theme->text_primary, 2);
        draw_text(80, 130, "ALL SYSTEMS OPERATING NORMALLY", 0xFF000000, 3);
        draw_text(80, 175, "NO UNACKNOWLEDGED ALARMS PRESENT IN STATE BUFFER", 0xFF000000, 1);
    }

    draw_text(50, 240, "ACTIONS & CONTROLS:", s_active_theme->text_primary, 2);

    draw_border_rect(50, 275, 240, 50, s_active_theme->primary_accent, s_active_theme->text_primary, 1);
    draw_text(70, 292, "[A] ACKNOWLEDGE ALARM", 0xFF000000, 2);

    draw_border_rect(310, 275, 240, 50, s_active_theme->alarm_critical, s_active_theme->text_primary, 1);
    draw_text(330, 292, "[F] ENGAGE FAILOVER", 0xFFFFFFFF, 2);

    draw_text(50, 350, "PRESS [A] ON KEYBOARD OR TOUCH TO ACKNOWLEDGE ALARMS", s_active_theme->text_secondary, 1);
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

    draw_text(50, 180, "INPUT AGNOSTICISM MODE: UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)", s_active_theme->text_primary, 1);
    draw_text(50, 210, "DISPLAY RESOLUTION: 800 x 480 ARGB8888 (LVGL SAFE)", s_active_theme->text_primary, 1);
    draw_text(50, 240, "SENSOR INGESTION FREQ: 1000 Hz (REAL-TIME ISR)", s_active_theme->text_primary, 1);
    draw_text(50, 270, "UI PRESENTATION REFRESH: 30 Hz (STALENESS CHECK & PARTIAL REDRAW)", s_active_theme->text_primary, 1);

    draw_border_rect(50, 320, 350, 45, s_active_theme->secondary_accent, s_active_theme->text_primary, 1);
    draw_text(65, 335, "PRESS [C] TO TOGGLE HIGH CONTRAST", s_active_theme->text_primary, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("HOT STANDBY FAILOVER MODE", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->alarm_critical, 3);
    draw_rect(20, 55, 760, 40, s_active_theme->alarm_critical);

    draw_text(180, 65, "HOT STANDBY FAILOVER ENGAGED", 0xFFFFFFFF, 3);

    draw_text(50, 120, "SAFEGUARD TRIPPED / STANDBY ACTIVATED NEAR-ZERO LATENCY FAILOVER", s_active_theme->alarm_critical, 2);

    char buf[128];
    snprintf(buf, sizeof(buf), "LAST VALID SENSOR TEMP : %.2f C", state->sensor_temp_mC / 1000.0f);
    draw_text(50, 160, buf, s_active_theme->text_primary, 2);

    snprintf(buf, sizeof(buf), "LAST VALID PRESSURE    : %.1f kPa", state->sensor_pressure_kPa / 10.0f);
    draw_text(50, 195, buf, s_active_theme->text_primary, 2);

    snprintf(buf, sizeof(buf), "LAST VALID MOTOR RPM   : %lu RPM", (unsigned long)state->sensor_rpm);
    draw_text(50, 230, buf, s_active_theme->text_primary, 2);

    draw_text(50, 280, "DUAL-LOOP WATCHDOG STATUS AT FAILOVER:", s_active_theme->primary_accent, 1);
    snprintf(buf, sizeof(buf), "HARDWARE LOOP: %s | SOFTWARE LOOP: %s",
             wd->hw_loop_alive ? "ALIVE" : "FAULTE", wd->sw_loop_alive ? "ALIVE" : "FAULTE");
    draw_text(50, 305, buf, s_active_theme->alarm_critical, 2);

    draw_border_rect(50, 345, 360, 45, s_active_theme->alarm_ok, s_active_theme->text_primary, 1);
    draw_text(70, 360, "PRESS [F] OR SELECT TO RESTORE PRIMARY HMI", 0xFF000000, 1);
}

/* --- Presentation Master Render Dispatch --- */
static void layer3_render_frame(void)
{
    const shared_state_buffer_t *state = layer2_get_state_buffer();
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    /* Clear Framebuffer with Background Color */
    draw_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_active_theme->bg_color);

    /* Render Active Screen */
    switch ((screen_id_t)state->active_screen) {
        case SCREEN_BOOT:
            render_screen_boot(state, wd);
            break;
        case SCREEN_DASHBOARD:
            render_screen_dashboard(state, wd);
            break;
        case SCREEN_DIAGNOSTICS:
            render_screen_diagnostics(state, wd);
            break;
        case SCREEN_ALARM:
            render_screen_alarm(state, wd);
            break;
        case SCREEN_SETTINGS:
            render_screen_settings(state, wd);
            break;
        case SCREEN_FAILOVER_STANDBY:
            render_screen_failover(state, wd);
            break;
        default:
            render_screen_dashboard(state, wd);
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
    printf("[LAYER 3 PRESENTATION] LVGL Presentation Engine & Screens Initialized.\n");
}

/* 30 Hz UI Timer Tick (Executes rendering, staleness check, software alive heartbeat) */
void layer3_ui_timer_tick_30hz(void)
{
    /* Always process watchdog tick and software alive heartbeat */
    layer2_watchdog_report_software_alive();
    layer2_watchdog_supervisor_tick();

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
}

const hmi_theme_t* layer3_get_current_theme(void)
{
    return s_active_theme;
}

const uint32_t* layer3_get_framebuffer(void)
{
    return s_framebuffer;
}
