/**
 * @file layer3_presentation.c
 * @brief Layer 3: LVGL Presentation Layer & Pre-Allocated Static Screens
 *
 * Renders into an 8bpp palettized plane owned by the display driver. Every
 * drawing call names a PAL_* slot rather than a literal colour, so switching
 * theme is a palette rewrite instead of a re-render.
 */

#include "../include/layer3_presentation.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include <stdio.h>
#include <string.h>

/* --- Framebuffer: one PAL_* index per pixel, owned by the display driver --- */
static uint8_t *s_fb = NULL;

/* --- Palette Definitions (0x00RRGGBB) --- */
static const uint32_t s_palette_dark[PAL_COUNT] = {
    [PAL_BG]         = 0x0B101D, /* Deep space navy */
    [PAL_CARD]       = 0x161F33, /* Dark slate card background */
    [PAL_PRIMARY]    = 0x00D2FF, /* Cyan glowing accent */
    [PAL_SECONDARY]  = 0x3B82F6, /* Electric blue */
    [PAL_TEXT]       = 0xF8FAFC, /* Bright crisp white */
    [PAL_TEXT_DIM]   = 0x94A3B8, /* Cool grey */
    [PAL_ALARM_CRIT] = 0xEF4444, /* Crimson red */
    [PAL_ALARM_OK]   = 0x10B981, /* Emerald green */
    [PAL_BLACK]      = 0x000000,
    [PAL_WHITE]      = 0xFFFFFF
};

static const uint32_t s_palette_contrast[PAL_COUNT] = {
    [PAL_BG]         = 0x000000, /* Pure black */
    [PAL_CARD]       = 0x1A1A1A, /* High contrast dark grey card */
    [PAL_PRIMARY]    = 0xFFFF00, /* Vibrant yellow */
    [PAL_SECONDARY]  = 0x00FFFF, /* Bright cyan */
    [PAL_TEXT]       = 0xFFFFFF, /* High contrast white */
    [PAL_TEXT_DIM]   = 0xE0E0E0, /* Light grey */
    [PAL_ALARM_CRIT] = 0xFF0000, /* Bright red */
    [PAL_ALARM_OK]   = 0x00FF00, /* Bright green */
    [PAL_BLACK]      = 0x000000,
    [PAL_WHITE]      = 0xFFFFFF
};

static const uint32_t *s_active_palette = s_palette_dark;
static uint32_t s_palette_revision = 1;

/*
 * Slot assignment is fixed for the life of the runtime; only the palette
 * contents change on a theme toggle. Kept as a struct so render code reads
 * semantically and so layer3_get_current_theme() stays a stable API.
 */
static hmi_theme_t s_theme = {
    .bg_color         = PAL_BG,
    .card_bg          = PAL_CARD,
    .primary_accent   = PAL_PRIMARY,
    .secondary_accent = PAL_SECONDARY,
    .text_primary     = PAL_TEXT,
    .text_secondary   = PAL_TEXT_DIM,
    .alarm_critical   = PAL_ALARM_CRIT,
    .alarm_ok         = PAL_ALARM_OK,
    .is_high_contrast = false
};

static const hmi_theme_t *s_active_theme = &s_theme;
static bool s_force_redraw = true;

/* --- Software Graphics Drawing Helpers (Pure C99) --- */
static void draw_rect(int x, int y, int w, int h, uint8_t color)
{
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > DISPLAY_WIDTH)  x2 = DISPLAY_WIDTH;
    if (y2 > DISPLAY_HEIGHT) y2 = DISPLAY_HEIGHT;
    if (x >= x2 || y >= y2) return;

    /* 1 byte per pixel: a scanline span is a single memset. */
    size_t span = (size_t)(x2 - x);
    for (int py = y; py < y2; py++) {
        memset(&s_fb[(size_t)py * DISPLAY_STRIDE + (size_t)x], color, span);
    }
}

static void draw_border_rect(int x, int y, int w, int h, uint8_t fill_color, uint8_t border_color, int border_thick)
{
    draw_rect(x, y, w, h, border_color);
    draw_rect(x + border_thick, y + border_thick, w - 2 * border_thick, h - 2 * border_thick, fill_color);
}

/*
 * 5x7 bitmap font, dense and contiguous over printable ASCII 0x20..0x7E.
 *
 * The previous sparse table used designated initializers with gaps and treated
 * "first column is blank" as "glyph undefined" -- which silently replaced 24 of
 * 95 printable characters (including '1', 'I', 'i', 'l', ':', '.', '(', ')',
 * '[', ']') with '?'. A dense table removes the sentinel entirely.
 */
#define FONT_FIRST 0x20
#define FONT_LAST  0x7E
#define FONT_COLS  5
#define FONT_ROWS  7
#define FONT_ADVANCE(scale) (((FONT_COLS) + 1) * (scale))

static const uint8_t font5x7[FONT_LAST - FONT_FIRST + 1][FONT_COLS] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */  {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */  {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */  {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */  {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */  {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */  {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */  {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */  {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */  {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */  {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */  {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */  {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */  {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */  {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */  {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */  {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */  {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */  {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */  {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */  {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */  {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */  {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */  {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */  {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */  {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */  {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */  {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */  {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */  {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */  {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\\' */ {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */  {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */  {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */  {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */  {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */  {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */  {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */  {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */  {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */  {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */  {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */  {0x48,0x54,0x54,0x54,0x24}, /* 's' */
    {0x04,0x3E,0x44,0x24,0x08}, /* 't' */  {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */  {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */  {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */  {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */  {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x08,0x08,0x2A,0x1C,0x08}  /* '~' */
};

static void draw_char(int x, int y, char c, uint8_t color, int scale)
{
    unsigned char uc = (unsigned char)c;
    if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
    const uint8_t *glyph = font5x7[uc - FONT_FIRST];

    for (int col = 0; col < FONT_COLS; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue; /* whole column blank: skip 7 tests */
        for (int row = 0; row < FONT_ROWS; row++) {
            if (bits & (1u << row)) {
                draw_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

int layer3_text_width(const char *str, int scale)
{
    if (!str) return 0;
    size_t n = strlen(str);
    if (n == 0) return 0;
    /* Last glyph contributes its 5 columns but not the trailing 1-column gap. */
    return (int)(n * (size_t)FONT_ADVANCE(scale)) - scale;
}

static void draw_text(int x, int y, const char *str, uint8_t color, int scale)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        draw_char(cur_x, y, *str, color, scale);
        cur_x += FONT_ADVANCE(scale);
        str++;
    }
}

/* Right-aligned text: x is the RIGHT edge. Keeps header widgets off each other. */
static void draw_text_right(int right_x, int y, const char *str, uint8_t color, int scale)
{
    draw_text(right_x - layer3_text_width(str, scale), y, str, color, scale);
}

/*
 * Draw at the largest scale that fits max_w, truncating with an ellipsis only
 * as a last resort. Nothing can silently overrun a neighbouring widget again.
 */
static void draw_text_fit(int x, int y, const char *str, uint8_t color, int max_w, int max_scale)
{
    char clipped[64];
    for (int scale = max_scale; scale >= 1; scale--) {
        if (layer3_text_width(str, scale) <= max_w) {
            draw_text(x, y, str, color, scale);
            return;
        }
    }
    /* Even scale 1 overflows: hard-truncate to the available cell. */
    int per_char = FONT_ADVANCE(1);
    int fits = (max_w + 1) / per_char;
    if (fits < 1) return;
    if (fits > (int)sizeof(clipped) - 1) fits = (int)sizeof(clipped) - 1;
    memcpy(clipped, str, (size_t)fits);
    clipped[fits] = '\0';
    if (fits >= 3) clipped[fits - 1] = '.';
    draw_text(x, y, clipped, color, 1);
}

/*
 * Bordered button with its label centred both ways and auto-scaled to fit.
 * Every on-screen button goes through here so none can overflow its box.
 */
static void draw_button(int x, int y, int w, int h, const char *label,
                        uint8_t fill_color, uint8_t text_color, int max_scale)
{
    draw_border_rect(x, y, w, h, fill_color, s_active_theme->secondary_accent, 1);

    int pad = 12;                     /* keep the glyphs off the border */
    int inner_w = w - 2 * pad;
    int scale = max_scale;
    while (scale > 1 && layer3_text_width(label, scale) > inner_w) scale--;

    int tw = layer3_text_width(label, scale);
    int th = FONT_ROWS * scale;
    int tx = x + (w - tw) / 2;
    if (tx < x + pad) tx = x + pad;
    draw_text_fit(tx, y + (h - th) / 2, label, text_color, inner_w, scale);
}

static void draw_progress_bar(int x, int y, int w, int h, float percent, uint8_t fill_color, uint8_t bg_color)
{
    draw_border_rect(x, y, w, h, bg_color, s_active_theme->secondary_accent, 1);
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 1.0f) percent = 1.0f;
    int fill_w = (int)((float)(w - 4) * percent);
    if (fill_w > 0) {
        draw_rect(x + 2, y + 2, fill_w, h - 4, fill_color);
    }
}

/* --- Header & Navigation Bar (Common across HMI screens) --- */

/* Header layout constants, laid out right-to-left so nothing collides. */
#define HDR_H          42
#define HDR_MARGIN     16
#define HDR_PILL_W     80
#define HDR_PILL_X     (DISPLAY_WIDTH - HDR_MARGIN - HDR_PILL_W)  /* 704 */
#define HDR_FRAME_R    (HDR_PILL_X - 12)                          /* 692 */
#define HDR_DIVIDER_X  340
#define HDR_TITLE_X    (HDR_DIVIDER_X + 16)
#define HDR_TITLE_MAXW (HDR_FRAME_R - 120 - HDR_TITLE_X)          /* worst-case counter */

static void draw_hmi_header(const char *screen_title, const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    /* Top Header Bar */
    draw_rect(0, 0, DISPLAY_WIDTH, HDR_H, s_active_theme->card_bg);
    draw_rect(0, HDR_H - 1, DISPLAY_WIDTH, 1, s_active_theme->primary_accent);

    draw_text(HDR_MARGIN, 12, "PROMETHEUS99 | HMI RUNTIME", s_active_theme->primary_accent, 2);
    /* Divider separates brand from screen title instead of a bare 16px gap. */
    draw_rect(HDR_DIVIDER_X, 8, 1, HDR_H - 16, s_active_theme->secondary_accent);
    draw_text_fit(HDR_TITLE_X, 15, screen_title, s_active_theme->text_primary, HDR_TITLE_MAXW, 2);

    /* Heartbeat Indicator Badge, right-aligned so long counters grow leftward */
    char hb_buf[32];
    snprintf(hb_buf, sizeof(hb_buf), "FRAME: %lu", (unsigned long)state->heartbeat_counter);
    draw_text_right(HDR_FRAME_R, 15, hb_buf, s_active_theme->text_secondary, 1);

    /* Watchdog Health Pill */
    uint8_t wd_color = wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;
    draw_border_rect(HDR_PILL_X, 10, HDR_PILL_W, 22, wd_color, s_active_theme->text_primary, 1);
    draw_text(HDR_PILL_X + 8, 14, wd->system_healthy ? "WD: OK" : "WD: TRIP", PAL_BLACK, 1);

    /* Bottom Navigation Bar */
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 38, s_active_theme->card_bg);
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 1, s_active_theme->secondary_accent);

    static const char *nav_labels[SCREEN_COUNT] = {
        "1:BOOT", "2:DASH", "3:DIAG", "4:ALARM", "5:CONFIG", "6:FAILOVER"
    };
    screen_id_t active = (screen_id_t)state->active_screen;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int bx = 10 + i * 120;
        bool on = (active == (screen_id_t)i);
        uint8_t fill = on ? ((i == SCREEN_FAILOVER_STANDBY) ? s_active_theme->alarm_critical
                                                           : s_active_theme->primary_accent)
                          : s_active_theme->card_bg;
        uint8_t text = on ? ((i == SCREEN_FAILOVER_STANDBY) ? PAL_WHITE : PAL_BLACK)
                          : s_active_theme->text_primary;
        draw_border_rect(bx, DISPLAY_HEIGHT - 32, 110, 26, fill, s_active_theme->secondary_accent, 1);
        /* Centre each label in its button instead of hand-tuned offsets. */
        int tw = layer3_text_width(nav_labels[i], 1);
        draw_text(bx + (110 - tw) / 2, DISPLAY_HEIGHT - 25, nav_labels[i], text, 1);
    }
}

/* --- Pre-Allocated Screen Renders --- */

static void render_screen_boot(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
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
    float temp_C = (float)state->sensor_temp_mC / 1000.0f;
    snprintf(val_buf, sizeof(val_buf), "%.2f C", temp_C);
    draw_text(35, 105, val_buf, s_active_theme->text_primary, 3);
    float temp_pct = (temp_C - 20.0f) / 60.0f;
    draw_progress_bar(35, 155, 205, 16, temp_pct, (temp_C > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, s_active_theme->bg_color);
    draw_text(35, 180, "NORMAL RANGE: 20.0 - 55.0 C", s_active_theme->text_secondary, 1);

    /* Card 2: Fieldbus Pressure */
    draw_border_rect(275, 55, 245, 175, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(290, 70, "PRESSURE", s_active_theme->primary_accent, 2);
    float press_kPa = (float)state->sensor_pressure_kPa / 10.0f;
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
    float bus_V = (float)state->sensor_bus_mv / 1000.0f;
    snprintf(val_buf, sizeof(val_buf), "BUS VOLTAGE: %.3f V", bus_V);
    draw_text(35, 295, val_buf, s_active_theme->text_primary, 2);

    draw_text(35, 330, "SENSOR ISR FREQ: 1000 Hz | UI TIMER: 30 Hz", s_active_theme->text_secondary, 1);
    draw_text(35, 355, "SERIALIZATION: PACKED C99 BINARY (CRC-16/CCITT)", s_active_theme->alarm_ok, 1);
    draw_text(35, 380, "FRAGMENTATION: 0% (STATIC ALLOCATION)", s_active_theme->primary_accent, 1);

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
    draw_hmi_header("DIAGNOSTICS", state, wd);

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

    snprintf(buf, sizeof(buf), "FRAME COUNTER: %lu | CRC-16/CCITT: 0x%04X",
             (unsigned long)state->heartbeat_counter, state->checksum);
    draw_text(35, 210, buf, s_active_theme->alarm_ok, 1);

    draw_rect(35, 240, 730, 2, s_active_theme->secondary_accent);

    draw_text(35, 255, "MEMORY ALLOCATION AUDIT (STATIC C99 BSS ARCHITECTURE)", s_active_theme->primary_accent, 2);

    /* One aligned label column: every row is scale 1 so the colons line up. */
    snprintf(buf, sizeof(buf), "%-26s: %d BYTES (%dx%d @ %d BPP)",
             "FRAMEBUFFER (INDEXED)", DISPLAY_BUF_SIZE,
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_COLOR_DEPTH);
    draw_text(35, 295, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-26s: %lu BYTES (PACKED C99 STRUCT)",
             "SHARED STATE BUFFER", (unsigned long)sizeof(shared_state_buffer_t));
    draw_text(35, 320, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-26s: %d OF 256 (THEME = PALETTE SWAP)",
             "PALETTE SLOTS IN USE", (int)PAL_COUNT);
    draw_text(35, 345, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-26s: 0 BYTES (ZERO FRAGMENTATION)",
             "DYNAMIC HEAP ALLOCATIONS");
    draw_text(35, 370, buf, s_active_theme->alarm_ok, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM MANAGEMENT", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, (state->alarm_severity != ALARM_NONE) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 2);

    draw_text(35, 70, "SYSTEM ALARM SUPERVISOR", s_active_theme->primary_accent, 2);

    if (state->alarm_severity != ALARM_NONE) {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_critical, s_active_theme->text_primary, 2);
        draw_text(80, 130, "CRITICAL TRIP CONDITION DETECTED", PAL_WHITE, 3);
        draw_text(80, 175, "SENSOR OVER-LIMIT OR VOLTAGE DROP RECORDED IN BINARY STATE", PAL_WHITE, 1);
    } else {
        draw_border_rect(50, 110, 700, 100, s_active_theme->alarm_ok, s_active_theme->text_primary, 2);
        draw_text(80, 130, "ALL SYSTEMS OPERATING NORMALLY", PAL_BLACK, 3);
        draw_text(80, 175, "NO UNACKNOWLEDGED ALARMS PRESENT IN STATE BUFFER", PAL_BLACK, 1);
    }

    draw_text(50, 240, "ACTIONS & CONTROLS:", s_active_theme->text_primary, 2);

    /* Two buttons span the card's inner width (50..750) with a 30px gutter. */
    draw_button(50, 275, 340, 54, "[A] ACKNOWLEDGE ALARM",
                s_active_theme->primary_accent, PAL_BLACK, 2);
    draw_button(420, 275, 340, 54, "[F] ENGAGE FAILOVER",
                s_active_theme->alarm_critical, PAL_WHITE, 2);

    draw_text(50, 355, "PRESS [A] ON KEYBOARD OR TOUCH TO ACKNOWLEDGE ALARMS", s_active_theme->text_secondary, 1);
}

static void render_screen_settings(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SETTINGS", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(35, 70, "HMI RUNTIME PREFERENCES", s_active_theme->primary_accent, 2);

    draw_text(50, 120, "THEME ACCESSIBILITY MODE:", s_active_theme->text_primary, 2);
    if (s_active_theme->is_high_contrast) {
        draw_button(400, 108, 340, 40, "HIGH CONTRAST [ON]",
                    s_active_theme->primary_accent, PAL_BLACK, 2);
    } else {
        draw_button(400, 108, 340, 40, "DARK MODE [ON]",
                    s_active_theme->card_bg, s_active_theme->text_primary, 2);
    }

    draw_text(50, 180, "INPUT AGNOSTICISM MODE: UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)", s_active_theme->text_primary, 1);
    draw_text(50, 210, "DISPLAY RESOLUTION: 800 x 480, 8BPP INDEXED (LVGL SAFE)", s_active_theme->text_primary, 1);
    draw_text(50, 240, "SENSOR INGESTION FREQ: 1000 Hz (REAL-TIME ISR)", s_active_theme->text_primary, 1);
    draw_text(50, 270, "UI PRESENTATION REFRESH: 30 Hz (DIRTY-FLAG GATED REDRAW)", s_active_theme->text_primary, 1);

    draw_button(50, 318, 360, 45, "PRESS [C] TO TOGGLE HIGH CONTRAST",
                s_active_theme->secondary_accent, PAL_WHITE, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("FAILOVER STANDBY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->alarm_critical, 3);
    draw_rect(20, 55, 760, 40, s_active_theme->alarm_critical);

    draw_text(180, 65, "HOT STANDBY FAILOVER ENGAGED", PAL_WHITE, 3);

    /* Was one 756px line from x=50 (ran 6px off an 800px panel); now two lines. */
    draw_text(50, 120, "SAFEGUARD TRIPPED - STANDBY ACTIVATED", s_active_theme->alarm_critical, 2);
    draw_text(50, 145, "NEAR-ZERO LATENCY FAILOVER PATH IS NOW SERVING THE HMI", s_active_theme->text_secondary, 1);

    char buf[128];
    snprintf(buf, sizeof(buf), "LAST VALID SENSOR TEMP : %.2f C", (double)state->sensor_temp_mC / 1000.0);
    draw_text(50, 180, buf, s_active_theme->text_primary, 2);

    snprintf(buf, sizeof(buf), "LAST VALID PRESSURE    : %.1f kPa", (double)state->sensor_pressure_kPa / 10.0);
    draw_text(50, 212, buf, s_active_theme->text_primary, 2);

    snprintf(buf, sizeof(buf), "LAST VALID MOTOR RPM   : %lu RPM", (unsigned long)state->sensor_rpm);
    draw_text(50, 244, buf, s_active_theme->text_primary, 2);

    draw_text(50, 285, "DUAL-LOOP WATCHDOG STATUS AT FAILOVER:", s_active_theme->primary_accent, 1);
    snprintf(buf, sizeof(buf), "HARDWARE LOOP: %s | SOFTWARE LOOP: %s",
             wd->hw_loop_alive ? "ALIVE" : "FAULT", wd->sw_loop_alive ? "ALIVE" : "FAULT");
    draw_text(50, 305, buf, s_active_theme->alarm_critical, 2);

    draw_button(50, 345, 400, 45, "PRESS [F] OR SELECT TO RESTORE PRIMARY HMI",
                s_active_theme->alarm_ok, PAL_BLACK, 1);
}

/* --- Presentation Master Render Dispatch --- */
static void layer3_render_frame(void)
{
    /* Take a consistent snapshot: the live buffer is mutated by the 1000 Hz ISR thread. */
    static shared_state_buffer_t state_snapshot;
    layer2_copy_state_buffer(&state_snapshot);
    const shared_state_buffer_t *state = &state_snapshot;
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    /* Clear Framebuffer with Background Color */
    draw_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_active_theme->bg_color);

    /* Render Active Screen */
    switch ((screen_id_t)state->active_screen) {
        case SCREEN_BOOT:              render_screen_boot(state, wd);        break;
        case SCREEN_DASHBOARD:         render_screen_dashboard(state, wd);   break;
        case SCREEN_DIAGNOSTICS:       render_screen_diagnostics(state, wd); break;
        case SCREEN_ALARM:             render_screen_alarm(state, wd);       break;
        case SCREEN_SETTINGS:          render_screen_settings(state, wd);    break;
        case SCREEN_FAILOVER_STANDBY:  render_screen_failover(state, wd);    break;
        default:                       render_screen_dashboard(state, wd);   break;
    }

    /* Flush to display via Layer 1 Display Flush Callback */
    display_area_t area = {0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1, NULL};
    layer1_display_flush_cb(&area, s_fb);
}

void layer3_presentation_init(uint8_t *fb)
{
    s_fb = fb;
    s_active_palette = s_palette_dark;
    s_theme.is_high_contrast = false;
    s_palette_revision++;
    s_force_redraw = true;
    if (s_fb) memset(s_fb, PAL_BG, (size_t)DISPLAY_BUF_SIZE);
    printf("[LAYER 3 PRESENTATION] Presentation engine ready (8bpp indexed, %d palette slots).\n",
           (int)PAL_COUNT);
}

bool layer3_ui_timer_tick_30hz(void)
{
    /* Drain HAL input queues. Producer (window proc) and consumer (this tick)
     * both run on the UI thread, so no locking is required. */
    input_key_t key;
    while (layer1_poll_button_event(&key)) {
        layer3_inject_input_key(key);
    }
    int16_t tx, ty;
    bool tpressed;
    while (layer1_poll_touch_event(&tx, &ty, &tpressed)) {
        layer3_handle_touch(tx, ty, tpressed);
    }

    /* Always process watchdog tick and software alive heartbeat */
    layer2_watchdog_report_software_alive();
    layer2_watchdog_supervisor_tick();

    /* Staleness check: only rasterize when telemetry or the UI actually moved. */
    bool dirty = layer2_consume_dirty_flag();
    if (!dirty && !s_force_redraw) {
        return false;
    }
    s_force_redraw = false;

    layer3_render_frame();
    return true;
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

    /* Bottom navigation bar: 6 buttons on a 120px pitch starting at x=10. */
    if (y >= (DISPLAY_HEIGHT - 38)) {
        int idx = (x - 10) / 120;
        int within = (x - 10) % 120;
        if (idx >= 0 && idx < SCREEN_COUNT && within < 110) {
            layer2_fsm_request_screen_change((screen_id_t)idx);
        }
    }
}

void layer3_toggle_high_contrast_theme(void)
{
    if (s_active_palette == s_palette_dark) {
        s_active_palette = s_palette_contrast;
        s_theme.is_high_contrast = true;
        printf("[LAYER 3 ACCESSIBILITY] Switched to High Contrast Theme.\n");
    } else {
        s_active_palette = s_palette_dark;
        s_theme.is_high_contrast = false;
        printf("[LAYER 3 ACCESSIBILITY] Switched to Sleek Dark Theme.\n");
    }
    /* The pixel plane is unchanged: only the palette the driver uploads differs. */
    s_palette_revision++;
    s_force_redraw = true;
}

const hmi_theme_t* layer3_get_current_theme(void)
{
    return s_active_theme;
}

const uint32_t* layer3_get_palette(void)
{
    return s_active_palette;
}

uint32_t layer3_get_palette_revision(void)
{
    return s_palette_revision;
}

const uint8_t* layer3_get_framebuffer(void)
{
    return s_fb;
}
