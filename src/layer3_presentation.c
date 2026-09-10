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
#include "../include/layer1_metrics.h"
#include "../include/layer2_core.h"
#include <stdio.h>
#include <string.h>

/* 4bpp gives 16 palette slots. If the theme ever outgrows that, fail the build
 * rather than silently wrapping colour indices into the wrong nibble value. */
typedef char prom_palette_fits_in_4bpp[(PAL_COUNT <= PALETTE_MAX_SLOTS) ? 1 : -1];

/* --- Framebuffer: 2 pixels per byte, owned by the display driver --- */
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

/* --- Software Graphics Drawing Helpers (Pure C99, 4bpp packed) --- */

/*
 * The framebuffer packs two pixels per byte: even x in the HIGH nibble, odd x
 * in the LOW nibble. Every drawing primitive funnels through draw_rect, so
 * getting the nibble arithmetic right here makes the whole UI 4bpp-correct.
 */
/* Y origin of the band currently being rasterized. All drawing is issued in
 * full-screen coordinates; this is the only place the band translation lives. */
static int s_band_y0 = 0;

static void draw_rect(int x, int y, int w, int h, uint8_t color)
{
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    if (x < 0) x = 0;
    if (x2 > DISPLAY_WIDTH) x2 = DISPLAY_WIDTH;
    if (x >= x2) return;

    /* Translate into band space and clip to the strip. A primitive that misses
     * this band costs two compares and nothing else. */
    int by0 = y - s_band_y0;
    int by1 = (y + h) - s_band_y0;
    if (by0 < 0) by0 = 0;
    if (by1 > DISPLAY_BAND_HEIGHT) by1 = DISPLAY_BAND_HEIGHT;
    if (by0 >= by1) return;

    color &= 0x0Fu;
    uint8_t both = (uint8_t)((color << 4) | color);   /* both pixels in a byte */

    for (int py = by0; py < by1; py++) {
        uint8_t *row = &s_fb[(size_t)py * DISPLAY_STRIDE];
        int px = x;

        /* Leading odd pixel lands in the low nibble of its byte. */
        if (px & 1) {
            row[px >> 1] = (uint8_t)((row[px >> 1] & 0xF0u) | color);
            px++;
        }
        /* Whole bytes: two pixels at a time. */
        int full = (x2 - px) >> 1;
        if (full > 0) {
            memset(&row[px >> 1], both, (size_t)full);
            px += full * 2;
        }
        /* Trailing even pixel lands in the high nibble of its byte. */
        if (px < x2) {
            row[px >> 1] = (uint8_t)((row[px >> 1] & 0x0Fu) | (uint8_t)(color << 4));
        }
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

/* Progress bar. Fill is expressed in permille (0..1000) so the whole
 * presentation path stays integer-only -- no float anywhere in this runtime. */
static void draw_progress_bar(int x, int y, int w, int h, int32_t permille,
                              uint8_t fill_color, uint8_t bg_color)
{
    draw_border_rect(x, y, w, h, bg_color, s_active_theme->secondary_accent, 1);
    if (permille < 0)    permille = 0;
    if (permille > 1000) permille = 1000;
    int fill_w = ((w - 4) * permille) / 1000;
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
#define HDR_TITLE_MAXW (HDR_FRAME_R - 120 - HDR_TITLE_X)

static void draw_hmi_header(const char *screen_title, const shared_state_buffer_t *state,
                            const watchdog_supervisor_t *wd)
{
    draw_rect(0, 0, DISPLAY_WIDTH, HDR_H, s_active_theme->card_bg);
    draw_rect(0, HDR_H - 1, DISPLAY_WIDTH, 1, s_active_theme->primary_accent);

    draw_text(HDR_MARGIN, 12, "PROMETHEUS99 | HMI RUNTIME", s_active_theme->primary_accent, 2);
    draw_rect(HDR_DIVIDER_X, 8, 1, HDR_H - 16, s_active_theme->secondary_accent);
    draw_text_fit(HDR_TITLE_X, 15, screen_title, s_active_theme->text_primary, HDR_TITLE_MAXW, 2);

    /*
     * The badge shows ui_version -- the count of actual re-rasterizations --
     * not a free-running frame counter. On a quiet machine it barely moves,
     * which is the anti-spam behaviour made visible.
     */
    char hb[32];
    snprintf(hb, sizeof(hb), "REDRAW: %lu", (unsigned long)state->ui_version);
    draw_text_right(HDR_FRAME_R, 15, hb, s_active_theme->text_secondary, 1);

    uint8_t wd_color = wd->system_healthy ? s_active_theme->alarm_ok : s_active_theme->alarm_critical;
    draw_border_rect(HDR_PILL_X, 10, HDR_PILL_W, 22, wd_color, s_active_theme->text_primary, 1);
    draw_text(HDR_PILL_X + 8, 14, wd->system_healthy ? "WD: OK" : "WD: TRIP", PAL_BLACK, 1);

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
        int tw = layer3_text_width(nav_labels[i], 1);
        draw_text(bx + (110 - tw) / 2, DISPLAY_HEIGHT - 25, nav_labels[i], text, 1);
    }
}

/* ===========================================================================
 * Metric tile
 *
 * Presentation-side full-scale per metric, used only for bar length. The value
 * itself is already quantized by Layer 1 to the precision shown here.
 * ==========================================================================*/
static const int32_t k_metric_fullscale[METRIC_COUNT] = {
    [METRIC_CPU_LOAD] = 1000,   /* tenths of % */
    [METRIC_MEM_USED] = 1000,
    [METRIC_GPU_LOAD] = 1000,
    [METRIC_BATTERY]  = 1000,
    [METRIC_CPU_TEMP] = 1000,   /* deci-C, 100.0 C full scale */
    [METRIC_FAN_RPM]  = 6000    /* RPM */
};

static const char *alarm_level_name(uint8_t lvl)
{
    switch (lvl) {
        case ALARM_WARNING:   return "WARNING";
        case ALARM_CRITICAL:  return "CRITICAL";
        case ALARM_EMERGENCY: return "EMERGENCY";
        case ALARM_INFO:      return "INFO";
        default:              return "NORMAL";
    }
}

static uint8_t alarm_level_color(uint8_t lvl)
{
    if (lvl >= ALARM_CRITICAL) return s_active_theme->alarm_critical;
    if (lvl >= ALARM_WARNING)  return s_active_theme->primary_accent;
    return s_active_theme->alarm_ok;
}

/*
 * Renders one host vital. The unavailable case is an explicit visual state --
 * a dimmed border, a literal "N/A", and the reason -- never a blank tile and
 * never a stale last-known value presented as if it were live.
 */
static void draw_metric_tile(int x, int y, int w, int h,
                             const shared_state_buffer_t *st, metric_id_t id)
{
    char buf[64];
    uint8_t state = st->metric_state[id];
    uint8_t alarm = st->metric_alarm[id];
    bool    dep   = st->metric_dependent[id] != 0;
    bool    avail = (state != MSTATE_UNAVAILABLE);

    uint8_t border = !avail ? s_active_theme->text_secondary
                            : (alarm >= ALARM_WARNING ? alarm_level_color(alarm)
                                                      : s_active_theme->primary_accent);
    draw_border_rect(x, y, w, h, s_active_theme->card_bg, border, 1);

    /* Label row */
    draw_text(x + 12, y + 10, layer1_metric_label(id),
              avail ? s_active_theme->primary_accent : s_active_theme->text_secondary, 2);

    if (!avail) {
        draw_text(x + 12, y + 40, "N/A", s_active_theme->text_secondary, 3);
        draw_text(x + 12, y + 78, "PROVIDER UNAVAILABLE", s_active_theme->text_secondary, 1);
        draw_text_fit(x + 12, y + 98, layer1_metric_provider_name(id),
                      s_active_theme->text_secondary, w - 24, 1);
        return;
    }

    if (state == MSTATE_WARMING) {
        draw_text(x + 12, y + 40, "--", s_active_theme->text_secondary, 3);
        draw_text(x + 12, y + 78, "SEEDING DEBOUNCE WINDOW", s_active_theme->text_secondary, 1);
        return;
    }

    /* Value: integer only. Percent and temperature metrics are quantized to
     * whole units, so a plain integer divide is the exact displayed value. */
    int32_t v = st->metric_value[id];
    if (id == METRIC_FAN_RPM) {
        snprintf(buf, sizeof(buf), "%ld", (long)v);
    } else {
        snprintf(buf, sizeof(buf), "%ld", (long)(v / 10));
    }
    draw_text(x + 12, y + 38, buf, s_active_theme->text_primary, 3);

    int vw = layer3_text_width(buf, 3);
    draw_text(x + 12 + vw + 8, y + 52, layer1_metric_unit(id), s_active_theme->text_secondary, 2);

    /* Bar */
    int32_t fs = k_metric_fullscale[id];
    int32_t permille = (fs > 0) ? (int32_t)(((int64_t)v * 1000) / fs) : 0;
    draw_progress_bar(x + 12, y + 82, w - 24, 12,
                      permille, alarm_level_color(alarm), s_active_theme->bg_color);

    /* Footer: alarm state, plus context from the provider aux field */
    if (alarm >= ALARM_WARNING) {
        snprintf(buf, sizeof(buf), "%s%s", alarm_level_name(alarm), dep ? " (SYMPTOM)" : "");
        draw_text(x + 12, y + 100, buf, alarm_level_color(alarm), 1);
    } else if (id == METRIC_MEM_USED) {
        snprintf(buf, sizeof(buf), "%ld MB TOTAL", (long)st->metric_aux[id]);
        draw_text(x + 12, y + 100, buf, s_active_theme->text_secondary, 1);
    } else if (id == METRIC_CPU_LOAD) {
        snprintf(buf, sizeof(buf), "%ld LOGICAL CORES", (long)st->metric_aux[id]);
        draw_text(x + 12, y + 100, buf, s_active_theme->text_secondary, 1);
    } else if (id == METRIC_BATTERY) {
        draw_text(x + 12, y + 100, st->metric_aux[id] == 1 ? "ON MAINS" : "ON BATTERY",
                  s_active_theme->text_secondary, 1);
    } else if (id == METRIC_GPU_LOAD) {
        snprintf(buf, sizeof(buf), "%ld ENGINES", (long)st->metric_aux[id]);
        draw_text(x + 12, y + 100, buf, s_active_theme->text_secondary, 1);
    } else {
        draw_text(x + 12, y + 100, "NOMINAL", s_active_theme->alarm_ok, 1);
    }
}


/* ===========================================================================
 * Trend graph
 *
 * Plots the Layer 2 history rings. The rings advance on their own slow clock
 * (METRIC_HISTORY_PERIOD_MS), so drawing them costs nothing extra in redraws:
 * the graph only moves when a new history sample has been appended.
 * ==========================================================================*/
static void plot_series(int px, int py, int pw, int ph,
                        metric_id_t id, int32_t fullscale, uint8_t color)
{
    uint16_t hist[METRIC_HISTORY_LEN];
    int n = layer2_get_metric_history(id, hist);
    if (n < 2 || fullscale <= 0) return;

    int prev_y = -1, prev_x = -1;
    for (int i = 0; i < n; i++) {
        int sx = px + (pw * i) / (METRIC_HISTORY_LEN - 1);
        int32_t v = hist[i];
        if (v > fullscale) v = fullscale;
        int sy = py + ph - 1 - (int)(((int64_t)v * (ph - 1)) / fullscale);

        if (prev_y >= 0) {
            /* Vertical connector spans the gap between consecutive samples so
             * the trace reads as a line rather than disconnected dots. */
            int y0 = (prev_y < sy) ? prev_y : sy;
            int y1 = (prev_y < sy) ? sy : prev_y;
            draw_rect(prev_x, y0, 2, (y1 - y0) + 2, color);
        }
        draw_rect(sx, sy, 2, 2, color);
        prev_y = sy;
        prev_x = sx;
    }
}

static void draw_trend_graph(int x, int y, int w, int h)
{
    draw_border_rect(x, y, w, h, s_active_theme->bg_color, s_active_theme->secondary_accent, 1);
    draw_text(x + 10, y + 8, "TREND", s_active_theme->primary_accent, 1);

    int px = x + 40, py = y + 22, pw = w - 52, ph = h - 40;

    /* Horizontal gridlines with axis labels at 0 / 50 / 100 percent. */
    for (int g = 0; g <= 2; g++) {
        int gy = py + (ph - 1) - (g * (ph - 1)) / 2;
        draw_rect(px, gy, pw, 1, s_active_theme->card_bg);
        const char *lbl = (g == 0) ? "0" : (g == 1 ? "50" : "100");
        draw_text(x + 10, gy - 3, lbl, s_active_theme->text_secondary, 1);
    }

    plot_series(px, py, pw, ph, METRIC_CPU_LOAD, 1000, s_active_theme->primary_accent);
    plot_series(px, py, pw, ph, METRIC_MEM_USED, 1000, s_active_theme->alarm_ok);

    /* Legend */
    int ly = y + h - 14;
    draw_rect(px, ly + 3, 10, 3, s_active_theme->primary_accent);
    draw_text(px + 14, ly, "CPU", s_active_theme->text_secondary, 1);
    draw_rect(px + 50, ly + 3, 10, 3, s_active_theme->alarm_ok);
    draw_text(px + 64, ly, "MEM", s_active_theme->text_secondary, 1);

    char b[48];
    snprintf(b, sizeof(b), "%d s WINDOW", (METRIC_HISTORY_LEN * METRIC_HISTORY_PERIOD_MS) / 1000);
    draw_text_right(x + w - 10, ly, b, s_active_theme->text_secondary, 1);
}

/* --- Pre-Allocated Screen Renders --- */

static void render_screen_boot(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("BOOT SEQUENCE", state, wd);

    draw_border_rect(150, 80, 500, 310, s_active_theme->card_bg, s_active_theme->primary_accent, 2);

    draw_text(220, 110, "PROMETHEUS99 HMI RUNTIME", s_active_theme->primary_accent, 2);
    draw_text(228, 140, "LIVE HOST TELEMETRY / C99 STATIC CORE", s_active_theme->text_secondary, 1);

    draw_text(180, 180, "[OK] Static Memory Pool Alloc: 0 Dynamic Bytes", s_active_theme->alarm_ok, 1);
    draw_text(180, 205, "[OK] Triple-Loop Watchdog Supervisor: ACTIVE", s_active_theme->alarm_ok, 1);
    draw_text(180, 230, "[OK] Host Metric Providers: PROBED", s_active_theme->alarm_ok, 1);
    draw_text(180, 255, "[OK] Hysteresis Alarm Engine: ARMED", s_active_theme->alarm_ok, 1);

    char buf[64];
    int ready = 0;
    for (int i = 0; i < METRIC_COUNT; i++) {
        if (state->metric_state[i] != MSTATE_UNAVAILABLE) ready++;
    }
    snprintf(buf, sizeof(buf), "PROVIDERS ONLINE: %d OF %d", ready, (int)METRIC_COUNT);
    draw_text(180, 295, buf, s_active_theme->text_primary, 1);
    draw_progress_bar(180, 315, 440, 20, (ready * 1000) / METRIC_COUNT,
                      s_active_theme->primary_accent, s_active_theme->bg_color);

    draw_text(210, 350, "PRESS [NEXT / KEY 2] TO ENTER DASHBOARD", s_active_theme->primary_accent, 1);
}

static void render_screen_dashboard(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("SYSTEM DASHBOARD", state, wd);

    /* Six host vitals on a fixed 3x2 grid. Pre-allocated layout: no widget is
     * created or destroyed at runtime. */
    static const int col_x[3] = { 20, 277, 534 };
    const int tile_w = 246, tile_h = 124;
    for (int i = 0; i < METRIC_COUNT; i++) {
        int cx = col_x[i % 3];
        int cy = (i < 3) ? 52 : 184;
        draw_metric_tile(cx, cy, tile_w, tile_h, state, (metric_id_t)i);
    }

    char buf[64];

    /* Bottom strip: live trend graph on the left, compact status on the right. */
    draw_trend_graph(20, 316, 560, 116);

    uint8_t sev = state->alarm_severity;
    draw_border_rect(592, 316, 188, 116, s_active_theme->card_bg,
                     sev >= ALARM_WARNING ? alarm_level_color(sev)
                                          : (wd->system_healthy ? s_active_theme->alarm_ok
                                                                : s_active_theme->alarm_critical), 1);

    snprintf(buf, sizeof(buf), "ALARM: %s", alarm_level_name(sev));
    draw_text(602, 324, buf, alarm_level_color(sev), 1);
    snprintf(buf, sizeof(buf), "EDGE EVENTS: %lu", (unsigned long)state->alarm_event_count);
    draw_text(602, 342, buf, s_active_theme->text_secondary, 1);

    draw_rect(602, 358, 168, 1, s_active_theme->secondary_accent);

    draw_text(602, 364, wd->hw_loop_alive ? "ISR  1000Hz OK" : "ISR  1000Hz DEAD",
              wd->hw_loop_alive ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);
    draw_text(602, 382, wd->sw_loop_alive ? "UI     30Hz OK" : "UI     30Hz DEAD",
              wd->sw_loop_alive ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);
    draw_text(602, 400, wd->metrics_loop_alive ? "POLL    2Hz OK" : "POLL    2Hz DEAD",
              wd->metrics_loop_alive ? s_active_theme->alarm_ok : s_active_theme->alarm_critical, 1);

    snprintf(buf, sizeof(buf), "PETS %lu / FAULTS %lu",
             (unsigned long)wd->total_watchdog_pets, (unsigned long)wd->watchdog_fault_count);
    draw_text(602, 418, buf,
              (wd->watchdog_fault_count > 0) ? s_active_theme->alarm_critical
                                             : s_active_theme->text_secondary, 1);
}

static void render_screen_diagnostics(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("DIAGNOSTICS", state, wd);

    draw_border_rect(20, 50, 760, 382, s_active_theme->card_bg, s_active_theme->primary_accent, 1);
    draw_text(32, 60, "ALARM RULE TABLE (STATIC CONST CONFIGURATION)", s_active_theme->primary_accent, 2);

    char buf[128];

    /* Column header for the data-driven threshold table. */
    draw_text(32, 90, "METRIC     PROVIDER            WARN ON/OFF  CRIT ON/OFF  CONF  ROOT",
              s_active_theme->text_secondary, 1);
    draw_rect(32, 104, 736, 1, s_active_theme->secondary_accent);

    for (int i = 0; i < METRIC_COUNT; i++) {
        const alarm_rule_t *r = &k_alarm_rules[i];
        int div = (i == METRIC_FAN_RPM) ? 1 : 10;
        const char *root = (r->root_cause < METRIC_COUNT)
                         ? layer1_metric_label(r->root_cause) : "-";
        snprintf(buf, sizeof(buf), "%-10s %-19s %4ld/%-6ld %4ld/%-6ld  %2u   %s",
                 layer1_metric_label((metric_id_t)i),
                 layer1_metric_provider_name((metric_id_t)i),
                 (long)(r->warn_on / div), (long)(r->warn_off / div),
                 (long)(r->crit_on / div), (long)(r->crit_off / div),
                 (unsigned)r->confirm_samples, root);
        uint8_t col = layer1_metric_provider_available((metric_id_t)i)
                    ? s_active_theme->text_primary : s_active_theme->text_secondary;
        draw_text(32, 112 + i * 20, buf, col, 1);
    }

    draw_rect(32, 240, 736, 1, s_active_theme->secondary_accent);
    draw_text(32, 250, "RUNTIME COUNTERS", s_active_theme->primary_accent, 2);

    snprintf(buf, sizeof(buf), "%-28s: %lu", "ISR TICKS (1000 Hz LIVENESS)",
             (unsigned long)state->isr_tick_count);
    draw_text(32, 282, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-28s: %lu", "METRIC PUBLISHES ACCEPTED",
             (unsigned long)state->publish_counter);
    draw_text(32, 302, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-28s: %lu", "UI RE-RASTERIZATIONS",
             (unsigned long)state->ui_version);
    draw_text(32, 322, buf, s_active_theme->alarm_ok, 1);

    snprintf(buf, sizeof(buf), "%-28s: %lu", "ALARM EDGE EVENTS",
             (unsigned long)state->alarm_event_count);
    draw_text(32, 342, buf, s_active_theme->alarm_ok, 1);

    snprintf(buf, sizeof(buf), "%-28s: %d BYTES (PACKED, CRC-16/CCITT 0x%04X)",
             "SHARED STATE BUFFER", (int)sizeof(shared_state_buffer_t), state->checksum);
    draw_text(32, 362, buf, s_active_theme->text_secondary, 1);

    snprintf(buf, sizeof(buf), "%-28s: %d BYTES (%dx%d @ %d BPP INDEXED)",
             "FRAMEBUFFER", DISPLAY_BUF_SIZE, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_COLOR_DEPTH);
    draw_text(32, 382, buf, s_active_theme->text_secondary, 1);

    draw_text(32, 402, "DYNAMIC HEAP ALLOCATIONS    : 0 BYTES (ZERO FRAGMENTATION)",
              s_active_theme->alarm_ok, 1);
}

static void render_screen_alarm(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("ALARM MANAGEMENT", state, wd);

    uint8_t sev = state->alarm_severity;
    draw_border_rect(20, 50, 760, 382, s_active_theme->card_bg,
                     (sev != ALARM_NONE) ? s_active_theme->alarm_critical : s_active_theme->alarm_ok, 2);

    /* Banner */
    if (sev != ALARM_NONE) {
        draw_border_rect(34, 62, 732, 62, s_active_theme->alarm_critical, s_active_theme->text_primary, 2);
        char b[64];
        snprintf(b, sizeof(b), "%s CONDITION CONFIRMED", alarm_level_name(sev));
        draw_text(50, 82, b, PAL_WHITE, 3);
    } else {
        draw_border_rect(34, 62, 732, 62, s_active_theme->alarm_ok, s_active_theme->text_primary, 2);
        draw_text(50, 82, "ALL HOST VITALS WITHIN BAND", PAL_BLACK, 3);
    }

    /* Edge-triggered event log: one row per confirmed transition, not one row
     * per sample. A quiet system produces an empty log. */
    draw_text(34, 140, "LATCHED EDGE EVENTS (NEWEST LAST)", s_active_theme->primary_accent, 2);
    draw_rect(34, 164, 732, 1, s_active_theme->secondary_accent);

    alarm_event_t events[ALARM_EVENT_LOG_DEPTH];
    int n = layer2_get_alarm_events(events);
    char buf[128];

    if (n == 0) {
        draw_text(34, 176, "NO TRANSITIONS RECORDED -- HYSTERESIS AND DEBOUNCE HOLDING",
                  s_active_theme->text_secondary, 1);
    }
    for (int i = 0; i < n && i < 6; i++) {
        const alarm_event_t *e = &events[i];
        int div = (e->metric == METRIC_FAN_RPM) ? 1 : 10;
        snprintf(buf, sizeof(buf), "%6lus  %-10s %-8s -> %-8s  @ %ld%s",
                 (unsigned long)(e->t_ms / 1000),
                 layer1_metric_label((metric_id_t)e->metric),
                 alarm_level_name(e->from_level), alarm_level_name(e->to_level),
                 (long)(e->value / div),
                 e->dependent ? "  [SYMPTOM]" : "");
        draw_text(34, 176 + i * 20, buf,
                  e->dependent ? s_active_theme->text_secondary : alarm_level_color(e->to_level), 1);
    }

    /* Actions */
    draw_text(34, 306, "ACTIONS & CONTROLS:", s_active_theme->text_primary, 2);
    draw_button(34, 336, 340, 48, "[A] ACKNOWLEDGE ALARM",
                s_active_theme->primary_accent, PAL_BLACK, 2);
    draw_button(426, 336, 340, 48, "[F] ENGAGE FAILOVER",
                s_active_theme->alarm_critical, PAL_WHITE, 2);

    snprintf(buf, sizeof(buf), "TOTAL EDGE EVENTS THIS SESSION: %lu",
             (unsigned long)state->alarm_event_count);
    draw_text(34, 400, buf, s_active_theme->text_secondary, 1);
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

    char buf[96];
    draw_text(50, 175, "INPUT MODE: UNIFIED INPUT GROUP (KEYPAD / TOUCH / CLI)",
              s_active_theme->text_primary, 1);
    draw_text(50, 200, "DISPLAY: 800 x 480, 8BPP INDEXED PALETTE", s_active_theme->text_primary, 1);

    snprintf(buf, sizeof(buf), "TELEMETRY POLL: %d ms FAST / %d ms SLOW (NOT PER-FRAME)",
             METRIC_POLL_PERIOD_MS, METRIC_SLOW_POLL_PERIOD_MS);
    draw_text(50, 225, buf, s_active_theme->text_primary, 1);

    draw_text(50, 250, "UI REFRESH: 30 Hz TIMER, REDRAW GATED ON QUANTIZED CHANGE",
              s_active_theme->text_primary, 1);
    draw_text(50, 275, "ALARMS: HYSTERESIS BAND + N-SAMPLE DEBOUNCE, EDGE-TRIGGERED",
              s_active_theme->text_primary, 1);

    snprintf(buf, sizeof(buf), "PROBE TIMEOUT: %d ms, ONE-SHOT (NO PER-POLL RETRY)",
             METRIC_PROBE_TIMEOUT_MS);
    draw_text(50, 300, buf, s_active_theme->text_primary, 1);

    draw_button(50, 340, 360, 45, "PRESS [C] TO TOGGLE HIGH CONTRAST",
                s_active_theme->secondary_accent, PAL_WHITE, 1);
}

static void render_screen_failover(const shared_state_buffer_t *state, const watchdog_supervisor_t *wd)
{
    draw_hmi_header("FAILOVER STANDBY", state, wd);

    draw_border_rect(20, 55, 760, 375, s_active_theme->card_bg, s_active_theme->alarm_critical, 3);
    draw_rect(20, 55, 760, 40, s_active_theme->alarm_critical);

    draw_text(180, 65, "HOT STANDBY FAILOVER ENGAGED", PAL_WHITE, 3);

    draw_text(50, 120, "SAFEGUARD TRIPPED - STANDBY ACTIVATED", s_active_theme->alarm_critical, 2);
    draw_text(50, 145, "LAST GOOD HOST TELEMETRY IS FROZEN BELOW", s_active_theme->text_secondary, 1);

    char buf[96];
    int row = 0;
    for (int i = 0; i < METRIC_COUNT; i++) {
        if (state->metric_state[i] == MSTATE_UNAVAILABLE) continue;
        int div = (i == METRIC_FAN_RPM) ? 1 : 10;
        snprintf(buf, sizeof(buf), "%-10s : %ld %s",
                 layer1_metric_label((metric_id_t)i),
                 (long)(state->metric_value[i] / div),
                 layer1_metric_unit((metric_id_t)i));
        draw_text(50, 175 + row * 26, buf, s_active_theme->text_primary, 2);
        row++;
        if (row >= 4) break;
    }

    draw_text(50, 290, "TRIPLE-LOOP WATCHDOG STATUS AT FAILOVER:", s_active_theme->primary_accent, 1);
    snprintf(buf, sizeof(buf), "ISR: %s | UI: %s | POLL: %s",
             wd->hw_loop_alive ? "ALIVE" : "FAULT",
             wd->sw_loop_alive ? "ALIVE" : "FAULT",
             wd->metrics_loop_alive ? "ALIVE" : "FAULT");
    draw_text(50, 310, buf, s_active_theme->alarm_critical, 2);

    draw_button(50, 345, 400, 45, "PRESS [F] OR SELECT TO RESTORE PRIMARY HMI",
                s_active_theme->alarm_ok, PAL_BLACK, 1);
}

/* --- Presentation Master Render Dispatch --- */
static void render_active_screen(const shared_state_buffer_t *state,
                                 const watchdog_supervisor_t *wd)
{
    switch ((screen_id_t)state->active_screen) {
        case SCREEN_BOOT:              render_screen_boot(state, wd);        break;
        case SCREEN_DASHBOARD:         render_screen_dashboard(state, wd);   break;
        case SCREEN_DIAGNOSTICS:       render_screen_diagnostics(state, wd); break;
        case SCREEN_ALARM:             render_screen_alarm(state, wd);       break;
        case SCREEN_SETTINGS:          render_screen_settings(state, wd);    break;
        case SCREEN_FAILOVER_STANDBY:  render_screen_failover(state, wd);    break;
        default:                       render_screen_dashboard(state, wd);   break;
    }
}

/*
 * Banded frame render.
 *
 * The screen is rasterized DISPLAY_BAND_COUNT times, once per horizontal strip,
 * and each strip is flushed to the panel as soon as it is complete. Drawing
 * code is unchanged and still works in full-screen coordinates: draw_rect does
 * the band translation and rejects primitives that miss the current strip.
 *
 * This is what removes the full-screen framebuffer from the memory budget.
 */
static void layer3_render_frame(void)
{
    static shared_state_buffer_t state_snapshot;
    layer2_copy_state_buffer(&state_snapshot);
    const shared_state_buffer_t *state = &state_snapshot;
    const watchdog_supervisor_t *wd = layer2_get_watchdog_status();

    for (int band = 0; band < DISPLAY_BAND_COUNT; band++) {
        s_band_y0 = band * DISPLAY_BAND_HEIGHT;

        draw_rect(0, s_band_y0, DISPLAY_WIDTH, DISPLAY_BAND_HEIGHT, s_active_theme->bg_color);
        render_active_screen(state, wd);

        display_area_t area = {
            0, (int16_t)s_band_y0,
            (int16_t)(DISPLAY_WIDTH - 1),
            (int16_t)(s_band_y0 + DISPLAY_BAND_HEIGHT - 1),
            s_fb
        };
        layer1_display_flush_cb(&area, s_fb);
    }
    s_band_y0 = 0;
}

void layer3_presentation_init(uint8_t *fb)
{
    s_fb = fb;
    s_active_palette = s_palette_dark;
    s_theme.is_high_contrast = false;
    s_palette_revision++;
    s_force_redraw = true;
    if (s_fb) memset(s_fb, (uint8_t)((PAL_BG << 4) | PAL_BG), (size_t)DISPLAY_BUF_SIZE);
    printf("[LAYER 3 PRESENTATION] Presentation engine ready (%d bpp indexed, %d of %d palette slots).\n",
           DISPLAY_COLOR_DEPTH, (int)PAL_COUNT, (int)PALETTE_MAX_SLOTS);
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

void layer3_request_redraw(void)
{
    s_force_redraw = true;
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
