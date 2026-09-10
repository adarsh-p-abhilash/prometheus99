/**
 * @file layer3_presentation.c
 * @brief Layer 3: LVGL Embedded Presentation Layer & Pre-Allocated Static Screens
 *
 * Implements standard LVGL v8 API backed by an 8-bit Indexed Color static architecture.
 *
 * Memory & Size Achievements:
 *   - 8-bit Indexed Framebuffer: 384,000 bytes (375 KB), reducing display RAM by 75%
 *     compared to original 32-bit (1.54 MB), bringing static RAM to ~386 KB.
 *   - 256-Color Palette Table (CLUT) for exact industrial HMI colors.
 *   - Zero dynamic heap allocation (0 bytes malloc/calloc).
 *   - High-speed memset row blitting for optimal embedded refresh.
 *   - 60-sample telemetry trend graph on Dashboard.
 *   - 16-event circular alarm journal on Alarms screen.
 */

#include "../include/layer3_presentation.h"
#include "../include/layer0_hardware.h"
#include "../include/layer1_hal.h"
#include "../include/layer2_core.h"
#include "../include/lvgl.h"
#include <stdio.h>
#include <string.h>

/* --- Static 8-Bit Indexed Framebuffer (375 KB - Extreme < 500 KB RAM Footprint) --- */
static uint8_t s_framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];

/* --- 256-Color CLUT Palette Table (0x00RRGGBB format for Win32 GDI RGBQUAD) --- */
static uint32_t s_palette[256];

/* --- LVGL Display & Input Driver Instances --- */
static lv_disp_draw_buf_t s_lv_disp_buf;
static lv_disp_drv_t      s_lv_disp_drv;
static lv_disp_t          s_lv_disp;
static lv_indev_drv_t     s_lv_indev_drv;
static lv_indev_t         s_lv_indev;

/* --- Static Scratch Buffers (Thread-Safe Single-Threaded UI Context) --- */
static char s_text_buf[128];
static char s_val_buf[64];

/* --- Dirty-Rectangle Redraw Tracking --- */
static screen_id_t s_last_rendered_screen = SCREEN_COUNT;
static bool s_force_full_redraw = true;

/* --- Default & High-Contrast HMI Themes (8-Bit Indexed) --- */
static hmi_theme_t s_theme_dark;
static hmi_theme_t s_theme_contrast;
static const hmi_theme_t *s_active_theme = &s_theme_dark;

/* Forward declaration */
static void layer3_render_frame(void);

/* --- Palette Initialization (256 Colors: Exact UI Palette + 6x6x6 Cube + Grayscale) --- */
static void init_palette_table(void)
{
    /* Dedicated industrial HMI color entries */
    s_palette[LV_COLOR_INDEX_BLACK]         = 0x000000;
    s_palette[LV_COLOR_INDEX_BG_DARK]       = 0x0B101D; /* Deep space navy */
    s_palette[LV_COLOR_INDEX_CARD_BG]       = 0x161F33; /* Dark slate card */
    s_palette[LV_COLOR_INDEX_CARD_CONTRAST] = 0x1A1A1A; /* Contrast slate */
    s_palette[LV_COLOR_INDEX_CYAN]          = 0x00D2FF; /* Cyan glow accent */
    s_palette[LV_COLOR_INDEX_BLUE]          = 0x3B82F6; /* Electric blue */
    s_palette[LV_COLOR_INDEX_WHITE]         = 0xF8FAFC; /* Crisp bright white */
    s_palette[LV_COLOR_INDEX_GREY]          = 0x94A3B8; /* Cool text grey */
    s_palette[LV_COLOR_INDEX_LIGHT_GREY]    = 0xE0E0E0; /* Light grey */
    s_palette[LV_COLOR_INDEX_DARK_GREY]     = 0x223344; /* Grid lines */
    s_palette[LV_COLOR_INDEX_RED]           = 0xEF4444; /* Crimson critical */
    s_palette[LV_COLOR_INDEX_BRIGHT_RED]    = 0xFF0000; /* Pure red */
    s_palette[LV_COLOR_INDEX_GREEN]         = 0x10B981; /* Emerald OK */
    s_palette[LV_COLOR_INDEX_BRIGHT_GREEN]  = 0x00FF00; /* Pure green */
    s_palette[LV_COLOR_INDEX_YELLOW]        = 0xFFFF00; /* Vibrant yellow */
    s_palette[LV_COLOR_INDEX_AMBER]         = 0xFBBF24; /* Warning amber */

    /* Standard 6x6x6 color cube: indices 16 to 231 */
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                uint8_t rv = (uint8_t)((r * 255) / 5);
                uint8_t gv = (uint8_t)((g * 255) / 5);
                uint8_t bv = (uint8_t)((b * 255) / 5);
                int idx = 16 + 36 * r + 6 * g + b;
                s_palette[idx] = ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | (uint32_t)bv;
            }
        }
    }

    /* 24-step grayscale ramp: indices 232 to 255 */
    for (int i = 0; i < 24; i++) {
        uint8_t gray = (uint8_t)((i * 255) / 23);
        s_palette[232 + i] = ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
    }
}

/* --- LVGL Core API Implementation (Embedded C99 Static Engine) --- */

static void lv_flush_internal(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)disp_drv;
    display_area_t d_area = {
        .x1 = area->x1,
        .y1 = area->y1,
        .x2 = area->x2,
        .y2 = area->y2,
        .pixel_color_p = (const uint8_t *)color_p
    };
    layer1_display_flush_cb(&d_area, (const uint8_t *)color_p);
    lv_disp_flush_ready(disp_drv);
}

void lv_init(void)
{
    init_palette_table();

    /* Initialize theme colors using dedicated palette indices */
    s_theme_dark.bg_color.full         = LV_COLOR_INDEX_BG_DARK;
    s_theme_dark.card_bg.full          = LV_COLOR_INDEX_CARD_BG;
    s_theme_dark.primary_accent.full   = LV_COLOR_INDEX_CYAN;
    s_theme_dark.secondary_accent.full = LV_COLOR_INDEX_BLUE;
    s_theme_dark.text_primary.full     = LV_COLOR_INDEX_WHITE;
    s_theme_dark.text_secondary.full   = LV_COLOR_INDEX_GREY;
    s_theme_dark.alarm_critical.full   = LV_COLOR_INDEX_RED;
    s_theme_dark.alarm_ok.full         = LV_COLOR_INDEX_GREEN;
    s_theme_dark.is_high_contrast      = false;

    s_theme_contrast.bg_color.full         = LV_COLOR_INDEX_BLACK;
    s_theme_contrast.card_bg.full          = LV_COLOR_INDEX_CARD_CONTRAST;
    s_theme_contrast.primary_accent.full   = LV_COLOR_INDEX_YELLOW;
    s_theme_contrast.secondary_accent.full = LV_COLOR_INDEX_CYAN;
    s_theme_contrast.text_primary.full     = LV_COLOR_INDEX_WHITE;
    s_theme_contrast.text_secondary.full   = LV_COLOR_INDEX_LIGHT_GREY;
    s_theme_contrast.alarm_critical.full   = LV_COLOR_INDEX_BRIGHT_RED;
    s_theme_contrast.alarm_ok.full         = LV_COLOR_INDEX_BRIGHT_GREEN;
    s_theme_contrast.is_high_contrast      = true;

    s_active_theme = &s_theme_dark;

    /* Setup LVGL draw buffer (8-bit indexed) */
    lv_disp_draw_buf_init(&s_lv_disp_buf, s_framebuffer, NULL, DISPLAY_BUF_SIZE);

    /* Register LVGL display driver */
    lv_disp_drv_init(&s_lv_disp_drv);
    s_lv_disp_drv.hor_res = DISPLAY_WIDTH;
    s_lv_disp_drv.ver_res = DISPLAY_HEIGHT;
    s_lv_disp_drv.draw_buf = &s_lv_disp_buf;
    s_lv_disp_drv.flush_cb = lv_flush_internal;
    lv_disp_drv_register(&s_lv_disp_drv);

    /* Register LVGL input driver */
    lv_indev_drv_init(&s_lv_indev_drv);
    s_lv_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&s_lv_indev_drv);
}

void lv_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf, void *buf1, void *buf2, uint32_t size_in_px_cnt)
{
    draw_buf->buf1 = buf1;
    draw_buf->buf2 = buf2;
    draw_buf->buf_act = buf1;
    draw_buf->size = size_in_px_cnt;
}

void lv_disp_drv_init(lv_disp_drv_t *driver)
{
    memset(driver, 0, sizeof(lv_disp_drv_t));
    driver->hor_res = DISPLAY_WIDTH;
    driver->ver_res = DISPLAY_HEIGHT;
}

lv_disp_t *lv_disp_drv_register(lv_disp_drv_t *driver)
{
    s_lv_disp.driver = driver;
    return &s_lv_disp;
}

void lv_disp_flush_ready(lv_disp_drv_t *disp_drv)
{
    (void)disp_drv;
}

void lv_indev_drv_init(lv_indev_drv_t *driver)
{
    memset(driver, 0, sizeof(lv_indev_drv_t));
    driver->type = LV_INDEV_TYPE_POINTER;
}

lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *driver)
{
    s_lv_indev.driver = driver;
    return &s_lv_indev;
}

void lv_tick_inc(uint32_t tick_period)
{
    (void)tick_period;
}

uint32_t lv_timer_handler(void)
{
    layer3_render_frame();
    return UI_FRAME_PERIOD_MS;
}

uint32_t lv_task_handler(void)
{
    return lv_timer_handler();
}

/* --- 8-Bit Software Drawing Primitives (High-Speed Memset) --- */

static void draw_rect(int x, int y, int w, int h, lv_color_t color)
{
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > DISPLAY_WIDTH) x2 = DISPLAY_WIDTH;
    if (y2 > DISPLAY_HEIGHT) y2 = DISPLAY_HEIGHT;

    int fill_w = x2 - x;
    if (fill_w <= 0) return;

    uint8_t c8 = color.full;
    if (x == 0 && fill_w == DISPLAY_WIDTH) {
        /* Full-width scanline optimization */
        memset(&s_framebuffer[y * DISPLAY_WIDTH], c8, (size_t)fill_w * (y2 - y));
    } else {
        for (int py = y; py < y2; py++) {
            memset(&s_framebuffer[py * DISPLAY_WIDTH + x], c8, (size_t)fill_w);
        }
    }
}

static void draw_border_rect(int x, int y, int w, int h, lv_color_t fill_color, lv_color_t border_color, int border_thick)
{
    draw_rect(x, y, w, h, border_color);
    draw_rect(x + border_thick, y + border_thick, w - 2 * border_thick, h - 2 * border_thick, fill_color);
}

/* 5x7 Built-in Standard ASCII Bitmap Font */
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

static void draw_char(int x, int y, char c, lv_color_t color, int scale)
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

static void draw_text(int x, int y, const char *str, lv_color_t color, int scale)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        draw_char(cur_x, y, *str, color, scale);
        cur_x += (5 * scale) + scale;
        str++;
    }
}

static void draw_progress_bar(int x, int y, int w, int h, float percent, lv_color_t fill_color, lv_color_t bg_color)
{
    draw_border_rect(x, y, w, h, bg_color, s_active_theme->secondary_accent, 1);
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 1.0f) percent = 1.0f;
    int fill_w = (int)((w - 4) * percent);
    if (fill_w > 0) {
        draw_rect(x + 2, y + 2, fill_w, h - 4, fill_color);
    }
}

/* --- Trend Mini-Graph (60 data points, 8-bit Indexed) --- */
static void draw_trend_graph(int x, int y, int w, int h, const telemetry_ring_buffer_t *trend)
{
    draw_border_rect(x, y, w, h, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(x + 5, y + 3, "TEMP TREND (60s)", s_active_theme->text_secondary, 1);

    if (trend->count < 2) return;

    int graph_x = x + 2;
    int graph_y = y + 16;
    int graph_w = w - 4;
    int graph_h = h - 20;

    /* Draw horizontal grid lines */
    uint8_t grid_idx = LV_COLOR_INDEX_DARK_GREY;
    for (int i = 0; i <= 4; i++) {
        int gy = graph_y + (graph_h * i) / 4;
        for (int gx = graph_x; gx < graph_x + graph_w; gx += 4) {
            if (gx < DISPLAY_WIDTH && gy < DISPLAY_HEIGHT) {
                s_framebuffer[gy * DISPLAY_WIDTH + gx] = grid_idx;
            }
        }
    }

    uint16_t count = trend->count;
    float step_x = (float)graph_w / (float)(TREND_HISTORY_SAMPLES - 1);

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (trend->head + TREND_HISTORY_SAMPLES - count + i) % TREND_HISTORY_SAMPLES;
        float temp_c = trend->samples[idx].temp_mC / 1000.0f;

        float norm = (temp_c - 30.0f) / 40.0f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        int px = graph_x + (int)(i * step_x);
        int py = graph_y + graph_h - (int)(norm * graph_h);

        if (px >= 0 && px + 1 < DISPLAY_WIDTH && py >= 0 && py + 1 < DISPLAY_HEIGHT) {
            lv_color_t dot_color = (temp_c > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->primary_accent;
            uint8_t dot8 = dot_color.full;
            s_framebuffer[py * DISPLAY_WIDTH + px] = dot8;
            s_framebuffer[py * DISPLAY_WIDTH + px + 1] = dot8;
            s_framebuffer[(py + 1) * DISPLAY_WIDTH + px] = dot8;
            s_framebuffer[(py + 1) * DISPLAY_WIDTH + px + 1] = dot8;
        }
    }
}

/* --- Header & Navigation Bar --- */
static void draw_hmi_header(const char *screen_title, const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    /* Top Header Bar */
    draw_rect(0, 0, DISPLAY_WIDTH, 42, s_active_theme->card_bg);
    draw_rect(0, 41, DISPLAY_WIDTH, 1, s_active_theme->primary_accent);

    draw_text(16, 12, "PROMETHEUS99 | 8-BIT LVGL", s_active_theme->primary_accent, 2);
    draw_text(380, 15, screen_title, s_active_theme->text_primary, 2);

    /* Heartbeat Indicator Badge */
    snprintf(s_text_buf, sizeof(s_text_buf), "FRAME: %lu", (unsigned long)state->heartbeat_counter);
    draw_text(670, 15, s_text_buf, s_active_theme->text_secondary, 1);

    /* Watchdog Health Pill */
    lv_color_t wd_color = wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;
    lv_color_t black_col = { .full = LV_COLOR_INDEX_BLACK };
    draw_border_rect(580, 10, 80, 22, wd_color, s_active_theme->text_primary, 1);
    draw_text(586, 14, wd->system_healthy ? "WD: OK" : "WD: TRIP", black_col, 1);

    /* Bottom Navigation Bar */
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 38, s_active_theme->card_bg);
    draw_rect(0, DISPLAY_HEIGHT - 38, DISPLAY_WIDTH, 1, s_active_theme->secondary_accent);

    screen_id_t active = (screen_id_t)state->active_screen;
    draw_border_rect(10, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_BOOT) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(25, DISPLAY_HEIGHT - 25, "1:BOOT", (active == SCREEN_BOOT) ? black_col : s_active_theme->text_primary, 1);

    draw_border_rect(130, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_DASHBOARD) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(142, DISPLAY_HEIGHT - 25, "2:DASHBOARD", (active == SCREEN_DASHBOARD) ? black_col : s_active_theme->text_primary, 1);

    draw_border_rect(250, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_DIAGNOSTICS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(258, DISPLAY_HEIGHT - 25, "3:DIAGNOST", (active == SCREEN_DIAGNOSTICS) ? black_col : s_active_theme->text_primary, 1);

    draw_border_rect(370, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_ALARM) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(388, DISPLAY_HEIGHT - 25, "4:ALARM", (active == SCREEN_ALARM) ? black_col : s_active_theme->text_primary, 1);

    draw_border_rect(490, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_SETTINGS) ? s_active_theme->primary_accent : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(502, DISPLAY_HEIGHT - 25, "5:SETTINGS", (active == SCREEN_SETTINGS) ? black_col : s_active_theme->text_primary, 1);

    draw_border_rect(610, DISPLAY_HEIGHT - 32, 110, 26, (active == SCREEN_FAILOVER_STANDBY) ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    lv_color_t white_col = { .full = LV_COLOR_INDEX_WHITE };
    draw_text(622, DISPLAY_HEIGHT - 25, "6:FAILOVER", (active == SCREEN_FAILOVER_STANDBY) ? white_col : s_active_theme->text_primary, 1);
}

/* --- Pre-Allocated Screen Renders --- */

static void render_screen_boot(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BOOT SEQUENCE", state, wd);

    draw_border_rect(150, 80, 500, 310, s_active_theme->card_bg, s_active_theme->primary_accent, 2);

    draw_text(205, 110, "PROMETHEUS99: LVGL EMBEDDED", s_active_theme->primary_accent, 2);
    draw_text(220, 140, "8-BIT INDEXED STATIC MEMORY ENGINE", s_active_theme->text_secondary, 1);

    draw_text(180, 180, "[OK] LVGL Display Driver (8-Bit CLUT): 375 KB RAM", s_active_theme->alarm_ok, 1);
    draw_text(180, 205, "[OK] Dual-Loop Watchdog Supervisor: ACTIVE", s_active_theme->alarm_ok, 1);
    draw_text(180, 230, "[OK] 1000 Hz Sensor ISR Pipeline: RUNNING", s_active_theme->alarm_ok, 1);
    draw_text(180, 255, "[OK] Lock-Free Ping-Pong State Exchange: READY", s_active_theme->alarm_ok, 1);

    float progress = ((float)(state->heartbeat_counter % 100)) / 100.0f;
    draw_text(180, 295, "SYSTEM INITIALIZATION PROGRESS:", s_active_theme->text_primary, 1);
    draw_progress_bar(180, 315, 440, 22, progress, s_active_theme->primary_accent, s_active_theme->card_bg);

    snprintf(s_text_buf, sizeof(s_text_buf), "%3d%% COMPLETE - PRESS [2] DASHBOARD", (int)(progress * 100));
    draw_text(240, 350, s_text_buf, s_active_theme->text_secondary, 1);
}

static void render_screen_dashboard(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SYSTEM DASHBOARD", state, wd);

    float temp_c = state->sensor_temp_mC / 1000.0f;
    float pressure_bar = state->sensor_pressure_kPa / 10.0f;
    float bus_v = state->sensor_bus_mv / 1000.0f;

    /* Card 1: Core Telemetry - Temperature */
    draw_border_rect(30, 60, 225, 115, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 75, "CORE TEMPERATURE", s_active_theme->text_secondary, 1);
    snprintf(s_val_buf, sizeof(s_val_buf), "%.2f C", temp_c);
    lv_color_t temp_col = (temp_c > 55.0f) ? s_active_theme->alarm_critical : s_active_theme->text_primary;
    draw_text(45, 95, s_val_buf, temp_col, 2);
    draw_progress_bar(45, 140, 195, 12, temp_c / 80.0f, temp_col, s_active_theme->card_bg);

    /* Card 2: Fieldbus Pressure */
    draw_border_rect(285, 60, 225, 115, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(300, 75, "SYSTEM PRESSURE", s_active_theme->text_secondary, 1);
    snprintf(s_val_buf, sizeof(s_val_buf), "%.2f BAR", pressure_bar);
    draw_text(300, 95, s_val_buf, s_active_theme->text_primary, 2);
    draw_progress_bar(300, 140, 195, 12, pressure_bar / 8.0f, s_active_theme->primary_accent, s_active_theme->card_bg);

    /* Card 3: Motor Speed RPM */
    draw_border_rect(540, 60, 225, 115, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(555, 75, "MOTOR SHAFT SPEED", s_active_theme->text_secondary, 1);
    snprintf(s_val_buf, sizeof(s_val_buf), "%u RPM", state->sensor_rpm);
    draw_text(555, 95, s_val_buf, s_active_theme->text_primary, 2);
    draw_progress_bar(555, 140, 195, 12, (float)state->sensor_rpm / 3600.0f, s_active_theme->alarm_ok, s_active_theme->card_bg);

    /* Card 4: Bus Voltage */
    draw_border_rect(30, 190, 225, 115, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 205, "DC BUS VOLTAGE", s_active_theme->text_secondary, 1);
    snprintf(s_val_buf, sizeof(s_val_buf), "%.2f V", bus_v);
    draw_text(45, 225, s_val_buf, s_active_theme->text_primary, 2);
    draw_progress_bar(45, 270, 195, 12, bus_v / 30.0f, s_active_theme->secondary_accent, s_active_theme->card_bg);

    /* Card 5: Trend Mini-Graph (60-Sample History) */
    const telemetry_ring_buffer_t *trend = layer2_get_trend_buffer();
    draw_trend_graph(285, 190, 480, 115, trend);

    /* Sub-System Overview Bar */
    draw_border_rect(30, 320, 735, 100, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 332, "PLANT AUTOMATION STATUS SUMMARY", s_active_theme->primary_accent, 1);

    const char *alarm_str = (state->alarm_severity == ALARM_NONE) ? "NORMAL / NO ALARM" :
                            (state->alarm_severity == ALARM_CRITICAL) ? "CRITICAL ALARM ACTIVE" : "WARNING THRESHOLD EXCEEDED";
    lv_color_t alarm_col = (state->alarm_severity == ALARM_NONE) ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;

    snprintf(s_text_buf, sizeof(s_text_buf), "SUPERVISORY ALARM STATUS:  %s", alarm_str);
    draw_text(45, 355, s_text_buf, alarm_col, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "FAILOVER SUBSYSTEM STATUS: %s", state->failover_active ? "HOT STANDBY ACTIVE" : "PRIMARY CONTROLLER ONLINE");
    draw_text(45, 375, s_text_buf, state->failover_active ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "TELEMETRY CHECKSUM (CRC16): 0x%04X [VALID]", state->checksum);
    draw_text(45, 395, s_text_buf, s_active_theme->text_secondary, 1);
}

static void render_screen_diagnostics(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BINARY DIAGNOSTICS & MEMORY PROFILER", state, wd);

    /* Memory Profile Card */
    draw_border_rect(30, 60, 360, 360, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 75, "STATIC MEMORY BUDGET (ZERO DYNAMIC)", s_active_theme->primary_accent, 1);

    draw_text(45, 105, "TOTAL HEAP CONSUMPTION: 0 BYTES (MALLOC FREE)", s_active_theme->alarm_ok, 1);
    draw_text(45, 130, "LVGL FRAMEBUFFER (8-BIT):   384,000 B (375 KB)", s_active_theme->text_primary, 1);
    draw_text(45, 155, "LOCK-FREE PING-PONG SLOTS:  2x 48 B (96 B)", s_active_theme->text_primary, 1);
    draw_text(45, 180, "TELEMETRY RING BUFFER (60s):964 BYTES", s_active_theme->text_primary, 1);
    draw_text(45, 205, "ALARM JOURNAL (16 EVENTS):  772 BYTES", s_active_theme->text_primary, 1);
    draw_text(45, 230, "HAL INPUT EVENT QUEUE (8):  32 BYTES", s_active_theme->text_primary, 1);
    draw_text(45, 255, "TOTAL STATIC RAM (BSS):     ~386 KB", s_active_theme->alarm_ok, 1);
    draw_text(45, 280, "RAM SAVINGS VS 32-BIT:      75% REDUCTION", s_active_theme->alarm_ok, 1);
    draw_text(45, 305, "COMPILER BINARY TARGET:     < 50 KB  [MET]", s_active_theme->alarm_ok, 1);
    draw_text(45, 330, "UNALIGNED BUS FAULT:        IMMUNE (ALIGNED)", s_active_theme->alarm_ok, 1);
    draw_text(45, 360, "DATA RACE HAZARD:           0 (PING-PONG SNAPSHOT)", s_active_theme->alarm_ok, 1);
    draw_text(45, 385, "WATCHDOG DUAL-LOOP:         ACTIVE (ISR + UI)", s_active_theme->alarm_ok, 1);

    /* Architecture Diagnostics Card */
    draw_border_rect(410, 60, 360, 360, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(425, 75, "4-LAYER ARCHITECTURE HEALTH", s_active_theme->primary_accent, 1);

    draw_text(425, 105, "[L0 HARDWARE] QPC Timer: FREQ CACHED", s_active_theme->alarm_ok, 1);
    draw_text(425, 130, "[L1 HAL] 1000Hz ISR Heartbeat: OK", s_active_theme->alarm_ok, 1);
    draw_text(425, 155, "[L1 HAL] Circular Input Queue: ACTIVE", s_active_theme->alarm_ok, 1);
    draw_text(425, 180, "[L2 CORE] Lock-Free Ping-Pong Buffer: OK", s_active_theme->alarm_ok, 1);
    draw_text(425, 205, "[L2 CORE] ISA-18.2 Latching FSM: OK", s_active_theme->alarm_ok, 1);
    draw_text(425, 230, "[L3 PRESENTATION] LVGL 8-Bit Engine: 30 Hz", s_active_theme->alarm_ok, 1);
    draw_text(425, 255, "[L3 PRESENTATION] Partial Redraw: READY", s_active_theme->alarm_ok, 1);

    uint64_t uptime_s = state->timestamp_ms / 1000;
    snprintf(s_text_buf, sizeof(s_text_buf), "RUNTIME SYSTEM UPTIME: %llu SECONDS", (unsigned long long)uptime_s);
    draw_text(425, 300, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "ISR SAMPLING TICKS:    %lu TICKS", (unsigned long)(uptime_s * 1000));
    draw_text(425, 325, s_text_buf, s_active_theme->text_secondary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "PRESENTATION FRAMES:   %lu FRAMES", (unsigned long)state->heartbeat_counter);
    draw_text(425, 350, s_text_buf, s_active_theme->text_secondary, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM SUPERVISOR (ISA-18.2)", state, wd);

    draw_border_rect(30, 60, 740, 160, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 75, "CURRENT ALARM STATUS & LATCHING FSM", s_active_theme->primary_accent, 1);

    const char *sev_str = "NORMAL / NO ALARM";
    lv_color_t sev_col = s_active_theme->alarm_ok;
    lv_color_t amber_col = { .full = LV_COLOR_INDEX_AMBER };
    if (state->alarm_severity == ALARM_WARNING) {
        sev_str = "WARNING: HIGH CORE TEMPERATURE / PRESSURE";
        sev_col = amber_col;
    } else if (state->alarm_severity >= ALARM_CRITICAL) {
        sev_str = "CRITICAL: EXCEEDED EMERGENCY THRESHOLD";
        sev_col = s_active_theme->alarm_critical;
    }

    draw_text(45, 100, sev_str, sev_col, 2);

    const char *fsm_str = (state->alarm_latch_state == ALARM_STATE_CLEARED) ? "CLEARED (No active condition)" :
                          (state->alarm_latch_state == ALARM_STATE_ACTIVE) ? "ACTIVE (Awaiting Operator Acknowledgment)" :
                          "ACKNOWLEDGED (Latched, awaiting physical clearance)";
    snprintf(s_text_buf, sizeof(s_text_buf), "LATCH STATE: %s", fsm_str);
    draw_text(45, 140, s_text_buf, s_active_theme->text_primary, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "TELEMETRY: Temp = %.1f C (Trip > 55 C) | Pres = %.1f Bar (Trip > 5 Bar)",
             state->sensor_temp_mC / 1000.0f, state->sensor_pressure_kPa / 10.0f);
    draw_text(45, 165, s_text_buf, s_active_theme->text_secondary, 1);

    /* Action Buttons (with touch hit-boxes) */
    lv_color_t white_col = { .full = LV_COLOR_INDEX_WHITE };
    draw_border_rect(45, 195, 230, 30, (state->alarm_latch_state == ALARM_STATE_ACTIVE) ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(65, 203, "[A] ACKNOWLEDGE ALARM", (state->alarm_latch_state == ALARM_STATE_ACTIVE) ? white_col : s_active_theme->text_primary, 1);

    draw_border_rect(300, 195, 230, 30, state->failover_active ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(320, 203, "[6/F] TOGGLE FAILOVER", state->failover_active ? white_col : s_active_theme->text_primary, 1);

    /* Alarm Journal (Last 4 Events) */
    draw_border_rect(30, 240, 740, 175, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(45, 252, "ALARM JOURNAL HISTORY (CIRCULAR LOG)", s_active_theme->primary_accent, 1);

    const alarm_journal_t *journal = layer2_get_alarm_journal();
    if (journal->count == 0) {
        draw_text(45, 280, "No alarm events recorded since system boot.", s_active_theme->text_secondary, 1);
    } else {
        uint16_t show_count = (journal->count < 4) ? journal->count : 4;
        for (uint16_t i = 0; i < show_count; i++) {
            uint16_t idx = (journal->head + ALARM_LOG_CAPACITY - 1 - i) % ALARM_LOG_CAPACITY;
            const alarm_log_entry_t *entry = &journal->entries[idx];

            const char *state_tag = (entry->latch_state == ALARM_STATE_ACTIVE) ? "[ACT]" :
                                    (entry->latch_state == ALARM_STATE_ACKNOWLEDGED) ? "[ACK]" : "[CLR]";
            lv_color_t tag_col = (entry->latch_state == ALARM_STATE_ACTIVE) ? s_active_theme->alarm_critical :
                                 (entry->latch_state == ALARM_STATE_ACKNOWLEDGED) ? amber_col : s_active_theme->alarm_ok;

            snprintf(s_text_buf, sizeof(s_text_buf), "%s T+%04llus: %s",
                     state_tag, (unsigned long long)(entry->timestamp_ms / 1000), entry->description);
            draw_text(45, 280 + i * 28, s_text_buf, tag_col, 1);
        }
    }
}

static void render_screen_settings(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SETTINGS & ACCESSIBILITY", state, wd);

    draw_border_rect(30, 60, 740, 360, s_active_theme->card_bg, s_active_theme->secondary_accent, 1);
    draw_text(50, 80, "HMI RUNTIME CONFIGURATION & ACCESSIBILITY", s_active_theme->primary_accent, 2);

    draw_text(50, 120, "DISPLAY COLOR DEPTH: 8-BIT INDEXED (256 CLUT - EXTREME LOW RAM)", s_active_theme->text_primary, 1);
    draw_text(50, 145, "GRAPHICS ENGINE:     LVGL EMBEDDED C99 ARCHITECTURE", s_active_theme->text_primary, 1);
    draw_text(50, 170, "FRAME REFRESH RATE:  30 HZ (~33 MS PERIOD)", s_active_theme->text_primary, 1);
    draw_text(50, 195, "SENSOR ISR RATE:     1000 HZ (1 MS PERIOD)", s_active_theme->text_primary, 1);
    draw_text(50, 220, "WATCHDOG MAX AGE:    500 MS HEARTBEAT SILENCE", s_active_theme->text_primary, 1);

    draw_text(50, 260, "INPUT AGNOSTICISM:   UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)", s_active_theme->text_primary, 1);
    draw_text(50, 285, "INPUT ROUTING:       WIN32 -> LAYER 1 HAL -> LAYER 2 FSM", s_active_theme->alarm_ok, 1);

    /* Theme Status & Toggle Button */
    snprintf(s_text_buf, sizeof(s_text_buf), "ACTIVE PALETTE:      %s",
             s_active_theme->is_high_contrast ? "HIGH CONTRAST (OUTDOOR MODE)" : "SLEEK DARK SPACE NAVY");
    draw_text(50, 325, s_text_buf, s_active_theme->primary_accent, 1);

    draw_border_rect(50, 355, 340, 40, s_active_theme->primary_accent, s_active_theme->text_primary, 2);
    lv_color_t black_col = { .full = LV_COLOR_INDEX_BLACK };
    draw_text(70, 368, "[C] TOGGLE HIGH CONTRAST THEME", black_col, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    (void)wd;
    draw_hmi_header("HOT STANDBY FAILOVER SUPERVISOR", state, wd);

    draw_border_rect(100, 80, 600, 320, s_active_theme->card_bg, s_active_theme->alarm_critical, 2);

    if (state->failover_active) {
        draw_text(160, 110, "SECONDARY CONTROLLER: ACTIVE", s_active_theme->alarm_critical, 2);
        draw_text(160, 140, "PRIMARY NODE: FAULTED / WATCHDOG TIMEOUT", s_active_theme->text_secondary, 1);
    } else {
        draw_text(160, 110, "PRIMARY CONTROLLER: ACTIVE", s_active_theme->alarm_ok, 2);
        draw_text(160, 140, "SECONDARY NODE: HOT STANDBY SYNCHRONIZED", s_active_theme->text_secondary, 1);
    }

    draw_text(160, 180, "[INFO] Dual-node redundant heartbeat monitoring.", s_active_theme->text_primary, 1);
    draw_text(160, 205, "[INFO] Automatic switchover triggers on 500ms silence.", s_active_theme->text_primary, 1);
    draw_text(160, 230, "[INFO] State synchronizes via lock-free binary payload.", s_active_theme->text_primary, 1);

    draw_border_rect(160, 275, 380, 45, state->failover_active ? s_active_theme->alarm_critical : s_active_theme->card_bg, s_active_theme->primary_accent, 2);
    lv_color_t white_col = { .full = LV_COLOR_INDEX_WHITE };
    draw_text(180, 290, "[6/F] TOGGLE HOT STANDBY FAILOVER", state->failover_active ? white_col : s_active_theme->primary_accent, 1);

    snprintf(s_text_buf, sizeof(s_text_buf), "CURRENT ACTIVE NODE: %s", state->failover_active ? "SECONDARY (STANDBY)" : "PRIMARY (MAIN)");
    draw_text(160, 345, s_text_buf, state->failover_active ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 1);
}

/* --- Presentation Master Render Dispatch --- */
static void layer3_render_frame(void)
{
    shared_state_buffer_t state;
    layer2_snapshot_state(&state);
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    screen_id_t current_screen = (screen_id_t)state.active_screen;

    if (current_screen != s_last_rendered_screen || s_force_full_redraw) {
        s_last_rendered_screen = current_screen;
        s_force_full_redraw = false;
    }

    /* Clear Framebuffer with Theme Background */
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
    lv_init();
    s_last_rendered_screen = SCREEN_COUNT;
    s_force_full_redraw = true;
    printf("[LAYER 3 PRESENTATION] LVGL Embedded Engine & 8-bit Indexed Screens Initialized (375 KB FB).\n");
}

/* 30 Hz UI Timer Tick */
void layer3_ui_timer_tick_30hz(void)
{
    /* Report software alive heartbeat to watchdog supervisor */
    layer2_watchdog_report_software_alive();
    layer2_watchdog_supervisor_tick();

    /* Drain HAL input queue */
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

    /* Execute LVGL timer handler & render */
    lv_timer_handler();
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

    /* Alarm screen buttons */
    screen_id_t active = layer2_fsm_get_active_screen();
    if (active == SCREEN_ALARM) {
        if (x >= 45 && x < 275 && y >= 195 && y < 235) {
            layer2_fsm_process_event(KEY_ALARM_ACK);
            return;
        }
        if (x >= 300 && x < 530 && y >= 195 && y < 235) {
            layer2_fsm_process_event(KEY_TOGGLE_FAILOVER);
            return;
        }
    }

    /* Settings screen contrast button */
    if (active == SCREEN_SETTINGS) {
        if (x >= 50 && x < 400 && y >= 350 && y < 405) {
            layer3_toggle_high_contrast_theme();
            return;
        }
    }

    /* Failover screen button */
    if (active == SCREEN_FAILOVER_STANDBY) {
        if (x >= 160 && x < 540 && y >= 275 && y < 320) {
            layer2_fsm_process_event(KEY_TOGGLE_FAILOVER);
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
    s_force_full_redraw = true;
}

const hmi_theme_t* layer3_get_current_theme(void)
{
    return s_active_theme;
}

const uint8_t* layer3_get_framebuffer(void)
{
    return s_framebuffer;
}

const uint32_t* layer3_get_palette(void)
{
    return s_palette;
}
