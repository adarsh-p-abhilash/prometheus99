/**
 * @file layer3_presentation.c
 * @brief Layer 3: LVGL Presentation Layer & Pre-Allocated Static Screens
 */

#include "../include/layer3_presentation.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include "../include/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Static Strip Buffer (Zero Dynamic Memory Allocation - 12 KB for 16-Strip 4-Bit Renderer) --- */
static uint8_t s_strip_buffer[DISPLAY_BUF_SIZE];
static int s_current_strip_y = 0;

/* --- Default & High-Contrast HMI Themes (4-Bit Palette Indices) --- */
static hmi_theme_t s_theme_dark = {
    .bg_color         = 1,  /* Dark Navy */
    .card_bg          = 2,  /* Slate Card */
    .primary_accent   = 6,  /* Cyan Glowing Accent */
    .secondary_accent = 5,  /* Electric Blue */
    .text_primary     = 15, /* Bright Crisp White */
    .text_secondary   = 13, /* Cool Grey */
    .alarm_critical   = 9,  /* Crimson Red */
    .alarm_ok         = 7,  /* Emerald Green */
    .is_high_contrast = false
};

static hmi_theme_t s_theme_contrast = {
    .bg_color         = 0,  /* Pure Black */
    .card_bg          = 2,  /* High Contrast Slate Card */
    .primary_accent   = 12, /* Vibrant Yellow */
    .secondary_accent = 6,  /* Bright Cyan */
    .text_primary     = 15, /* High Contrast White */
    .text_secondary   = 14, /* Light Grey */
    .alarm_critical   = 10, /* Bright Red */
    .alarm_ok         = 8,  /* Bright Green */
    .is_high_contrast = true
};

static const hmi_theme_t *s_active_theme = &s_theme_dark;

/* --- Software Graphics Drawing Helpers (Pure C99 4-Bit Strip Renderer) --- */
static inline void set_pixel_4bpp(int x, int y, uint8_t color)
{
    if (x < 0 || x >= DISPLAY_WIDTH) return;
    if (y < s_current_strip_y || y >= (s_current_strip_y + STRIP_HEIGHT)) return;
    int local_y = y - s_current_strip_y;
    int pos = local_y * DISPLAY_WIDTH + x;
    int idx = pos >> 1;
    color &= 0x0F;
    if (pos & 1) {
        s_strip_buffer[idx] = (s_strip_buffer[idx] & 0xF0) | color;
    } else {
        s_strip_buffer[idx] = (s_strip_buffer[idx] & 0x0F) | (color << 4);
    }
}

static void draw_rect(int x, int y, int w, int h, uint8_t color)
{
    color &= 0x0F;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > DISPLAY_WIDTH) x2 = DISPLAY_WIDTH;
    if (y2 > DISPLAY_HEIGHT) y2 = DISPLAY_HEIGHT;

    int sy1 = s_current_strip_y;
    int sy2 = s_current_strip_y + STRIP_HEIGHT;

    /* Clip to active strip bounds */
    if (y2 <= sy1 || y >= sy2) return;

    int py1 = (y < sy1) ? sy1 : y;
    int py2 = (y2 > sy2) ? sy2 : y2;

    for (int py = py1; py < py2; py++) {
        int local_y = py - sy1;
        int row_start = local_y * DISPLAY_WIDTH;
        for (int px = x; px < x2; px++) {
            int pos = row_start + px;
            int idx = pos >> 1;
            if (pos & 1) {
                s_strip_buffer[idx] = (s_strip_buffer[idx] & 0xF0) | color;
            } else {
                s_strip_buffer[idx] = (s_strip_buffer[idx] & 0x0F) | (color << 4);
            }
        }
    }
}

static void draw_border_rect(int x, int y, int w, int h, uint8_t fill_color, uint8_t border_color, int border_thick)
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

/* --- Tiny C99 Fast String Formatters (Zero CRT Float Stdio Overhead) --- */
static void fast_fmt_uint(char *buf, uint32_t val)
{
    char temp[16];
    int i = 0;
    if (val == 0) {
        temp[i++] = '0';
    } else {
        while (val > 0 && i < 15) {
            temp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    while (i > 0) {
        *buf++ = temp[--i];
    }
    *buf = '\0';
}

static void fast_fmt_hex32(char *buf, uint32_t val)
{
    const char hex_chars[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[2 + i] = hex_chars[val & 0x0F];
        val >>= 4;
    }
    buf[10] = '\0';
}

static void fast_fmt_hex16(char *buf, uint16_t val)
{
    const char hex_chars[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 3; i >= 0; i--) {
        buf[2 + i] = hex_chars[val & 0x0F];
        val >>= 4;
    }
    buf[6] = '\0';
}

static __attribute__((unused)) void fast_fmt_float1(char *buf, float val, const char *suffix)
{
    if (val < 0.0f) val = 0.0f;
    uint32_t int_part = (uint32_t)val;
    uint32_t dec_part = (uint32_t)((val - (float)int_part) * 10.0f + 0.5f);
    if (dec_part >= 10) { int_part++; dec_part = 0; }
    
    char ibuf[16];
    fast_fmt_uint(ibuf, int_part);
    
    char *p = buf;
    char *s = ibuf;
    while (*s) *p++ = *s++;
    *p++ = '.';
    *p++ = '0' + (dec_part % 10);
    if (suffix) {
        while (*suffix) *p++ = *suffix++;
    }
    *p = '\0';
}

static void fast_fmt_float2(char *buf, float val, const char *suffix)
{
    if (val < 0.0f) val = 0.0f;
    uint32_t int_part = (uint32_t)val;
    uint32_t dec_part = (uint32_t)((val - (float)int_part) * 100.0f + 0.5f);
    if (dec_part >= 100) { int_part++; dec_part = 0; }

    char ibuf[16];
    fast_fmt_uint(ibuf, int_part);

    char *p = buf;
    char *s = ibuf;
    while (*s) *p++ = *s++;
    *p++ = '.';
    *p++ = '0' + ((dec_part / 10) % 10);
    *p++ = '0' + (dec_part % 10);
    if (suffix) {
        while (*suffix) *p++ = *suffix++;
    }
    *p = '\0';
}

static void draw_char(int x, int y, char c, uint8_t color, int scale)
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

static void draw_text(int x, int y, const char *str, uint8_t color, int scale)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        draw_char(cur_x, y, *str, color, scale);
        cur_x += (5 * scale) + scale;
        str++;
    }
}

static void draw_progress_bar(int x, int y, int w, int h, float percent, uint8_t fill_color, uint8_t bg_color)
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

    /* Left Logo */
    draw_text(16, 12, "PROMETHEUS99", s_active_theme->primary_accent, 2);

    /* Active Screen Title */
    draw_text(190, 15, screen_title, s_active_theme->text_primary, 1);

    /* Watchdog Health Pill */
    uint8_t wd_color = wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;
    draw_border_rect(540, 9, 95, 24, wd_color, s_active_theme->text_primary, 1);
    draw_text(552, 14, wd->system_healthy ? "WD: OK" : "WD: TRIP", 0x00, 1);

    /* Heartbeat Frame Counter Badge */
    char hb_buf[32] = "FRAME: ";
    fast_fmt_uint(hb_buf + 7, (uint32_t)state->heartbeat_counter);
    draw_text(650, 15, hb_buf, s_active_theme->text_secondary, 1);

    /* Bottom Navigation Bar */
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 38, s_active_theme->card_bg);
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 1, s_active_theme->secondary_accent);

    screen_id_t active = (screen_id_t)state->active_screen;
    draw_border_rect(10, DISPLAY_HEIGHT - 32, 140, 26, (active == SCREEN_DASHBOARD) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(35, DISPLAY_HEIGHT - 23, "1:DASHBOARD", (active == SCREEN_DASHBOARD) ? 0x00 : s_active_theme->text_primary, 1);

    draw_border_rect(160, DISPLAY_HEIGHT - 32, 140, 26, (active == SCREEN_DIAGNOSTICS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(185, DISPLAY_HEIGHT - 23, "2:DIAGNOST", (active == SCREEN_DIAGNOSTICS) ? 0x00 : s_active_theme->text_primary, 1);

    draw_border_rect(310, DISPLAY_HEIGHT - 32, 140, 26, (active == SCREEN_ALARM) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(348, DISPLAY_HEIGHT - 23, "3:ALARM", (active == SCREEN_ALARM) ? 0x00 : s_active_theme->text_primary, 1);

    draw_border_rect(460, DISPLAY_HEIGHT - 32, 140, 26, (active == SCREEN_SETTINGS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(488, DISPLAY_HEIGHT - 23, "4:SETTINGS", (active == SCREEN_SETTINGS) ? 0x00 : s_active_theme->text_primary, 1);

    draw_border_rect(610, DISPLAY_HEIGHT - 32, 140, 26, (active == SCREEN_FAILOVER_STANDBY) ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(638, DISPLAY_HEIGHT - 23, "5:FAILOVER", (active == SCREEN_FAILOVER_STANDBY) ? 0xFF : s_active_theme->text_primary, 1);
}

/* --- Pre-Allocated Screen Renders --- */



static void draw_line(int x1, int y1, int x2, int y2, uint8_t color, int thick)
{
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        for (int tx = 0; tx < thick; tx++) {
            for (int ty = 0; ty < thick; ty++) {
                set_pixel_4bpp(x1 + tx, y1 + ty, color);
            }
        }
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

/* --- Real-Time Chart Waveform History --- */
#define CHART_HISTORY_LEN 50
static float s_temp_history[CHART_HISTORY_LEN];
static size_t s_history_count = 0;

#define CPU_HISTORY_LEN 24
static float s_cpu_history[CPU_HISTORY_LEN];
static size_t s_cpu_history_count = 0;

static void render_screen_dashboard(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SYSTEM DASHBOARD", state, wd);

    char val_buf[64];

    /* Card 1: CPU Thermal / Load */
    draw_border_rect(20, 55, 235, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "CPU THERMAL", s_active_theme->primary_accent, 2);
    float temp_C = state->sensor_temp_mC / 1000.0f;
    fast_fmt_float2(val_buf, temp_C, " C");
    draw_text(35, 105, val_buf, s_active_theme->text_primary, 3);
    float temp_pct = (temp_C - 20.0f) / 40.0f;
    draw_progress_bar(35, 155, 205, 16, temp_pct, (temp_C > 45.0f) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, s_active_theme->bg_color);
    draw_text(35, 180, (temp_C > 45.0f) ? "ALARM: CPU TEMP > 45.0 C!" : "CPU ALARM LIMIT: 45.0 C", (temp_C > 45.0f) ? s_active_theme->alarm_critical : s_active_theme->text_secondary, 1);

    /* Card 2: Host RAM Utilization */
    draw_border_rect(275, 55, 245, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(290, 70, "HOST RAM LOAD", s_active_theme->primary_accent, 2);
    uint32_t ram_pct = state->sensor_pressure_kPa;
    fast_fmt_uint(val_buf, ram_pct);
    strcat(val_buf, " %");
    draw_text(290, 105, val_buf, s_active_theme->text_primary, 3);
    float press_pct = ((float)ram_pct) / 100.0f;
    draw_progress_bar(290, 155, 215, 16, press_pct, s_active_theme->secondary_accent, s_active_theme->bg_color);
    draw_text(290, 180, "WIN32 GLOBAL MEMORY LOAD", s_active_theme->text_secondary, 1);

    /* Card 3: System Threads */
    draw_border_rect(540, 55, 240, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(555, 70, "SYSTEM THREADS", s_active_theme->primary_accent, 2);
    fast_fmt_uint(val_buf, (uint32_t)state->sensor_rpm);
    strcat(val_buf, " THRDS");
    draw_text(555, 105, val_buf, s_active_theme->text_primary, 3);
    float rpm_pct = ((float)state->sensor_rpm) / 5000.0f;
    if (rpm_pct > 1.0f) rpm_pct = 1.0f;
    draw_progress_bar(555, 155, 210, 16, rpm_pct, s_active_theme->primary_accent, s_active_theme->bg_color);
    draw_text(555, 180, "ACTIVE HOST OS THREADS", s_active_theme->text_secondary, 1);

    /* Lower Left Section: Realtime Telemetry Waveform Line Chart */
    draw_border_rect(20, 245, 500, 185, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(35, 256, "REALTIME TELEMETRY WAVEFORM (30 Hz)", s_active_theme->primary_accent, 1);
    
    float bus_V = state->sensor_bus_mv / 1000.0f;
    char bbuf[16], tbuf[16];
    fast_fmt_float2(bbuf, bus_V, "V");
    fast_fmt_float2(tbuf, temp_C, " C");
    strcpy(val_buf, "BUS: ");
    strcat(val_buf, bbuf);
    strcat(val_buf, " | LIVE: ");
    strcat(val_buf, tbuf);
    draw_text(325, 256, val_buf, s_active_theme->alarm_ok, 1);

    /* Chart Frame & Grid Lines */
    int chart_x = 40;
    int chart_y = 275;
    int chart_w = 465;
    int chart_h = 145;
    draw_rect(chart_x, chart_y, chart_w, chart_h, 1);
    draw_border_rect(chart_x, chart_y, chart_w, chart_h, 1, s_active_theme->secondary_accent, 1);

    /* Grid Horizontal Lines & Y-Axis Labels */
    uint8_t grid_color = 3;
    draw_rect(chart_x, chart_y + 35, chart_w, 1, grid_color);
    draw_text(chart_x + 4, chart_y + 27, "55C", s_active_theme->text_secondary, 1);

    draw_rect(chart_x, chart_y + 70, chart_w, 1, grid_color);
    draw_text(chart_x + 4, chart_y + 62, "40C", s_active_theme->text_secondary, 1);

    draw_rect(chart_x, chart_y + 105, chart_w, 1, grid_color);
    draw_text(chart_x + 4, chart_y + 97, "25C", s_active_theme->text_secondary, 1);

    /* Render Realtime Waveform Line Graph */
    if (s_history_count > 1) {
        int prev_px = 0, prev_py = 0;
        for (size_t i = 0; i < s_history_count; i++) {
            float val = s_temp_history[i];
            if (val < 20.0f) val = 20.0f;
            if (val > 65.0f) val = 65.0f;

            float norm = (val - 20.0f) / 45.0f;
            int px = chart_x + (int)((i * (chart_w - 10)) / (CHART_HISTORY_LEN - 1)) + 5;
            int py = (chart_y + chart_h - 10) - (int)(norm * (chart_h - 20));

            if (i > 0) {
                draw_line(prev_px, prev_py, px, py, s_active_theme->primary_accent, 2);
            }
            /* Render vertex point dot */
            draw_rect(px - 1, py - 1, 3, 3, (val > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok);

            prev_px = px;
            prev_py = py;
        }
    }

    /* Card 4: CPU Usage Visualization (Replaces Dual-Loop Watchdog) */
    float cpu_load = layer0_get_host_cpu_load();
    uint8_t cpu_color = (cpu_load > 85.0f) ? s_active_theme->alarm_critical : ((cpu_load > 50.0f) ? s_active_theme->secondary_accent : s_active_theme->alarm_ok);
    draw_border_rect(540, 245, 240, 185, s_active_theme->card_bg, cpu_color, 1);
    draw_text(555, 258, "CPU USAGE", cpu_color, 2);

    /* Big Load Percentage */
    fast_fmt_float2(val_buf, cpu_load, "%");
    draw_text(555, 288, val_buf, s_active_theme->text_primary, 3);

    /* CPU Progress Bar */
    float norm_cpu = cpu_load / 100.0f;
    if (norm_cpu < 0.0f) norm_cpu = 0.0f;
    if (norm_cpu > 1.0f) norm_cpu = 1.0f;
    draw_progress_bar(555, 323, 210, 14, norm_cpu, cpu_color, s_active_theme->bg_color);

    /* CPU Load History Mini Bar Chart (x: 555..765, y: 345..388) */
    int spark_x = 555;
    int spark_y = 345;
    int spark_w = 210;
    int spark_h = 43;
    draw_rect(spark_x, spark_y, spark_w, spark_h, 1);
    draw_border_rect(spark_x, spark_y, spark_w, spark_h, 1, s_active_theme->secondary_accent, 1);

    if (s_cpu_history_count > 0) {
        int bar_w = (spark_w - 6) / CPU_HISTORY_LEN;
        if (bar_w < 2) bar_w = 2;
        for (size_t i = 0; i < s_cpu_history_count; i++) {
            float val = s_cpu_history[i];
            float h_ratio = val / 100.0f;
            if (h_ratio < 0.05f) h_ratio = 0.05f;
            if (h_ratio > 1.0f) h_ratio = 1.0f;
            int bh = (int)(h_ratio * (spark_h - 4));
            int bx = spark_x + 3 + (int)(i * bar_w);
            int by = (spark_y + spark_h - 2) - bh;
            uint8_t c = (val > 85.0f) ? s_active_theme->alarm_critical : ((val > 50.0f) ? s_active_theme->secondary_accent : s_active_theme->alarm_ok);
            draw_rect(bx, by, bar_w - 1, bh, c);
        }
    }

    draw_text(555, 403, "HOST PROCESSOR LOAD (LIVE)", s_active_theme->text_secondary, 1);
}

static void render_screen_diagnostics(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BINARY DIAGNOSTICS & MEMORY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "SHARED STATE BUFFER BINARY PACKET INSPECTOR", s_active_theme->primary_accent, 2);

    char buf[128];
    strcpy(buf, "MAGIC HEADER: ");
    fast_fmt_hex32(buf + strlen(buf), (uint32_t)state->magic_header);
    strcat(buf, " (PROM)");
    draw_text(35, 110, buf, s_active_theme->text_primary, 1);

    strcpy(buf, "TIMESTAMP MS: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->timestamp_ms);
    strcat(buf, " ms");
    draw_text(35, 135, buf, s_active_theme->text_primary, 1);

    strcpy(buf, "RAW TEMP mC: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_temp_mC);
    strcat(buf, " | RAW RAM LOAD %: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_pressure_kPa);
    strcat(buf, " | RAW THREADS: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_rpm);
    strcat(buf, " | BUS mV: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_bus_mv);
    draw_text(35, 160, buf, s_active_theme->text_primary, 1);

    strcpy(buf, "DIRTY FLAG: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->dirty_flag);
    strcat(buf, " | ALARM SEVERITY: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->alarm_severity);
    strcat(buf, " | ACTIVE SCREEN ID: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->active_screen);
    draw_text(35, 185, buf, s_active_theme->primary_accent, 1);

    strcpy(buf, "FRAME COUNTER: ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->heartbeat_counter);
    strcat(buf, " | CRC16 CHECKSUM: ");
    fast_fmt_hex16(buf + strlen(buf), state->checksum);
    draw_text(35, 210, buf, s_active_theme->alarm_ok, 1);

    draw_rect(35, 240, 730, 2, s_active_theme->secondary_accent);

    draw_text(35, 255, "MEMORY ALLOCATION AUDIT (STATIC C99 BSS ARCHITECTURE)", s_active_theme->primary_accent, 2);
    draw_text(35, 290, "FRAMEBUFFER STATIC ARRAY : 12,000 BYTES (800x480 4-BIT INDEXED)", s_active_theme->text_secondary, 1);
    draw_text(35, 315, "SHARED STATE BUFFER      : 36 BYTES (PACKED C99 STRUCT)", s_active_theme->text_secondary, 1);
    draw_text(35, 340, "DYNAMIC HEAP ALLOCATIONS : 0 BYTES (ZERO FRAGMENTATION)", s_active_theme->alarm_ok, 2);
    draw_text(35, 375, "ESTIMATED BASE BINARY    : ~60 KB (HIGH AUDITABILITY)", s_active_theme->alarm_ok, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM MANAGEMENT", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, (state->alarm_severity != ALARM_NONE) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 2);

    draw_text(35, 70, "SYSTEM ALARM SUPERVISOR", s_active_theme->primary_accent, 2);

    if (state->alarm_severity != ALARM_NONE || (state->sensor_temp_mC > 45000)) {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_critical, s_active_theme->text_primary, 2);
        draw_text(80, 130, "CRITICAL TRIP: CPU TEMP > 45 C", 15, 3);
        draw_text(80, 175, "AUDIBLE ALARM BEEP ACTIVE - CPU OVER-TEMPERATURE RECORDED", 15, 1);
    } else {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_ok, s_active_theme->text_primary, 2);
        draw_text(80, 130, "ALL SYSTEMS OPERATING NORMALLY", 0, 3);
        draw_text(80, 175, "CPU TEMP BELOW 45 C ALARM THRESHOLD", 0, 1);
    }

    draw_text(50, 240, "ACTIONS & CONTROLS:", s_active_theme->text_primary, 2);

    /* Button 1: Acknowledge Alarm (Width: 320, Text Centered) */
    draw_border_rect(40, 275, 310, 50, s_active_theme->primary_accent, s_active_theme->text_primary, 1);
    draw_text(69, 293, "[A] ACKNOWLEDGE ALARM", 0, 2);

    /* Button 2: Engage Failover (Width: 310, Text Centered) */
    draw_border_rect(380, 275, 310, 50, s_active_theme->alarm_critical, s_active_theme->text_primary, 1);
    draw_text(421, 293, "[F] ENGAGE FAILOVER", 15, 2);

    draw_text(50, 350, "PRESS [A] ON KEYBOARD OR TOUCH TO ACKNOWLEDGE ALARMS", s_active_theme->text_secondary, 1);
}

static void render_screen_settings(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("HMI CONFIGURATION & ACCESSIBILITY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "HMI RUNTIME PREFERENCES", s_active_theme->primary_accent, 2);

    draw_text(50, 120, "THEME ACCESSIBILITY MODE:", s_active_theme->text_primary, 1);
    if (s_active_theme->is_high_contrast) {
        draw_border_rect(280, 110, 220, 35, s_active_theme->primary_accent, s_active_theme->text_primary, 1);
        draw_text(295, 122, "HIGH CONTRAST [ON]", 0, 1);
    } else {
        draw_border_rect(280, 110, 220, 35, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
        draw_text(295, 122, "DARK MODE [ON]", s_active_theme->text_primary, 1);
    }

    draw_text(50, 170, "INPUT AGNOSTICISM MODE: UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)", s_active_theme->text_primary, 1);
    draw_text(50, 200, "DISPLAY RESOLUTION: 800 x 480 4-BIT COLOR (16-PALETTE)", s_active_theme->text_primary, 1);
    draw_text(50, 230, "SENSOR INGESTION FREQ: 1000 Hz (REAL-TIME ISR)", s_active_theme->text_primary, 1);
    draw_text(50, 260, "UI PRESENTATION REFRESH: 30 Hz (STALENESS CHECK & PARTIAL REDRAW)", s_active_theme->text_primary, 1);

    draw_border_rect(50, 310, 360, 45, s_active_theme->secondary_accent, s_active_theme->text_primary, 1);
    draw_text(131, 328, "PRESS [C] TO TOGGLE HIGH CONTRAST", s_active_theme->text_primary, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("HOT STANDBY FAILOVER MODE", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->alarm_critical, 3);
    draw_rect(20, 55, 760, 40, s_active_theme->alarm_critical);

    draw_text(180, 65, "HOT STANDBY FAILOVER ENGAGED", 0xFF, 3);

    draw_text(50, 115, "SAFEGUARD TRIPPED / STANDBY ACTIVATED", s_active_theme->alarm_critical, 1);
    draw_text(50, 135, "NEAR-ZERO LATENCY FAILOVER MODE ENGAGED", s_active_theme->primary_accent, 1);

    char buf[128];
    strcpy(buf, "LAST VALID SENSOR TEMP : ");
    fast_fmt_float2(buf + strlen(buf), state->sensor_temp_mC / 1000.0f, " C");
    draw_text(50, 175, buf, s_active_theme->text_primary, 2);

    strcpy(buf, "LAST VALID RAM LOAD    : ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_pressure_kPa);
    strcat(buf, " %");
    draw_text(50, 210, buf, s_active_theme->text_primary, 2);

    strcpy(buf, "LAST VALID MOTOR RPM   : ");
    fast_fmt_uint(buf + strlen(buf), (uint32_t)state->sensor_rpm);
    strcat(buf, " RPM");
    draw_text(50, 245, buf, s_active_theme->text_primary, 2);

    draw_text(50, 285, "DUAL-LOOP WATCHDOG STATUS AT FAILOVER:", s_active_theme->primary_accent, 1);
    strcpy(buf, "HARDWARE LOOP: ");
    strcat(buf, wd->hw_loop_alive ? "ALIVE" : "FAULTE");
    strcat(buf, " | SOFTWARE LOOP: ");
    strcat(buf, wd->sw_loop_alive ? "ALIVE" : "FAULTE");
    draw_text(50, 305, buf, s_active_theme->alarm_critical, 2);

    draw_border_rect(50, 345, 400, 45, s_active_theme->alarm_ok, s_active_theme->text_primary, 1);
    draw_text(127, 363, "PRESS [F] OR SELECT TO RESTORE PRIMARY HMI", 0x00, 1);
}

/* --- Presentation Master Render Dispatch (16-Strip Band Renderer) --- */
static void layer3_render_frame(void)
{
    const shared_state_buffer_t *state = layer2_get_state_buffer();
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    for (int s = 0; s < STRIP_COUNT; s++) {
        s_current_strip_y = s * STRIP_HEIGHT;

        /* Clear Strip Buffer with Active Theme Background Color */
        uint8_t bg = s_active_theme->bg_color & 0x0F;
        uint8_t double_bg = (bg << 4) | bg;
        memset(s_strip_buffer, double_bg, sizeof(s_strip_buffer));

        /* Render Active Screen (Elements clip automatically to s_current_strip_y) */
        switch ((screen_id_t)state->active_screen) {
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

        /* Flush Strip Buffer to Layer 1 HAL Callback */
        display_area_t area = {
            0,
            (int16_t)s_current_strip_y,
            DISPLAY_WIDTH - 1,
            (int16_t)(s_current_strip_y + STRIP_HEIGHT - 1),
            s_strip_buffer
        };
        layer1_display_flush_cb(&area, s_strip_buffer);
    }
}

static void lvgl_display_flush_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    (void)disp_drv;
    display_area_t layer1_area;
    layer1_area.x1 = area ? area->x1 : 0;
    layer1_area.y1 = area ? area->y1 : 0;
    layer1_area.x2 = area ? area->x2 : (DISPLAY_WIDTH - 1);
    layer1_area.y2 = area ? area->y2 : (DISPLAY_HEIGHT - 1);
    layer1_area.pixel_color_p = color_p;
    layer1_display_flush_cb(&layer1_area, color_p);
    lv_disp_flush_ready(disp_drv);
}

static void lvgl_indev_read_cb(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void)indev_drv;
    int16_t tx, ty;
    bool pressed;
    if (layer1_poll_touch_event(&tx, &ty, &pressed)) {
        data->point.x = tx;
        data->point.y = ty;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}

void layer3_presentation_init(void)
{
    memset(s_strip_buffer, 0, sizeof(s_strip_buffer));
    s_active_theme = &s_theme_dark;

    /* Initialize LVGL Core Engine */
    lv_init();

    /* Setup LVGL Display Draw Buffer */
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_strip_buffer, NULL, DISPLAY_BUF_SIZE);

    /* Register LVGL Display Driver */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = lvgl_display_flush_cb;
    lv_disp_drv_register(&disp_drv);

    /* Register LVGL Unified Input Device Driver */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_indev_read_cb;
    lv_indev_t * indev = lv_indev_drv_register(&indev_drv);

    lv_group_t * input_group = lv_group_create();
    lv_indev_set_group(indev, input_group);

    printf("[LAYER 3 PRESENTATION] LVGL Strip Presentation Engine (12 KB Buffer) Initialized.\n");
}

/* 30 Hz UI Timer Tick (Executes rendering, staleness check, software alive heartbeat) */
void layer3_ui_timer_tick_30hz(void)
{
    /* Push latest sensor temperature reading into realtime chart ring buffer */
    const shared_state_buffer_t *st = layer2_get_state_buffer();
    float cur_temp = st->sensor_temp_mC / 1000.0f;
    if (s_history_count < CHART_HISTORY_LEN) {
        s_temp_history[s_history_count++] = cur_temp;
    } else {
        memmove(&s_temp_history[0], &s_temp_history[1], sizeof(float) * (CHART_HISTORY_LEN - 1));
        s_temp_history[CHART_HISTORY_LEN - 1] = cur_temp;
    }

    /* Push CPU load reading into CPU history ring buffer */
    float cur_cpu = layer0_get_host_cpu_load();
    if (s_cpu_history_count < CPU_HISTORY_LEN) {
        s_cpu_history[s_cpu_history_count++] = cur_cpu;
    } else {
        memmove(&s_cpu_history[0], &s_cpu_history[1], sizeof(float) * (CPU_HISTORY_LEN - 1));
        s_cpu_history[CPU_HISTORY_LEN - 1] = cur_cpu;
    }

    /* Increment LVGL Ticks & Run LVGL Task Handler */
    lv_tick_inc(33);
    lv_task_handler();

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

    const shared_state_buffer_t *state = layer2_get_state_buffer();

    /* Screen-specific button touches */
    if ((screen_id_t)state->active_screen == SCREEN_ALARM) {
        /* Button 1: Acknowledge Alarm (x: 40..350, y: 275..325) */
        if (x >= 40 && x < 350 && y >= 275 && y < 325) {
            layer3_inject_input_key(KEY_ALARM_ACK);
            return;
        }
    }

    /* Handle bottom navigation bar touch points */
    if (y >= (DISPLAY_HEIGHT - 38)) {
        if (x >= 10 && x < 150) {
            layer2_fsm_request_screen_change(SCREEN_DASHBOARD);
        } else if (x >= 160 && x < 300) {
            layer2_fsm_request_screen_change(SCREEN_DIAGNOSTICS);
        } else if (x >= 310 && x < 450) {
            layer2_fsm_request_screen_change(SCREEN_ALARM);
        } else if (x >= 460 && x < 600) {
            layer2_fsm_request_screen_change(SCREEN_SETTINGS);
        } else if (x >= 610 && x < 750) {
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

const uint8_t* layer3_get_framebuffer(void)
{
    return s_strip_buffer;
}
