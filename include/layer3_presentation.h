/**
 * @file layer3_presentation.h
 * @brief Layer 3: LVGL Presentation & Pre-Allocated Static Screens
 * 
 * Implements LVGL widget presentation, unified input group handling,
 * 30 Hz refresh timer with staleness monitoring, and static screen memory.
 */

#ifndef LAYER3_PRESENTATION_H
#define LAYER3_PRESENTATION_H

#include "config.h"
#include "layer2_core.h"

/*
 * --- High Contrast / Colour Palette Theme ---
 * Fields are palette SLOT INDICES, not colours. The slots are fixed for the
 * life of the runtime; the theme toggle rewrites what each slot looks like.
 */
typedef struct {
    uint8_t bg_color;
    uint8_t card_bg;
    uint8_t primary_accent;
    uint8_t secondary_accent;
    uint8_t text_primary;
    uint8_t text_secondary;
    uint8_t alarm_critical;
    uint8_t alarm_ok;
    bool is_high_contrast;
} hmi_theme_t;

/* --- Function Declarations --- */
/* fb points at the one-band partial draw buffer owned by the display driver. */
void layer3_presentation_init(uint8_t *fb);

/*
 * 30 Hz UI Timer Tick: drains HAL input queues, reports software alive, runs the
 * watchdog check, and re-renders only when the state buffer is dirty.
 * Returns true when the framebuffer changed and the driver should present it.
 */
bool layer3_ui_timer_tick_30hz(void);

/* Force a full banded repaint on the next tick (e.g. after WM_PAINT, which has
 * no full-screen backing store to blit from under banded rendering). */
void layer3_request_redraw(void);

/* Input Group Processing */
void layer3_inject_input_key(input_key_t key);
void layer3_handle_touch(int16_t x, int16_t y, bool pressed);

/* Theme & Contrast Toggle */
void layer3_toggle_high_contrast_theme(void);
const hmi_theme_t* layer3_get_current_theme(void);

/*
 * Active palette as 0x00RRGGBB, PAL_COUNT entries. The revision counter bumps
 * on every theme change so the display driver knows when to reload the DIB
 * colour table; nothing needs to be redrawn.
 */
const uint32_t* layer3_get_palette(void);
uint32_t layer3_get_palette_revision(void);

/* Text metrics (advance is 6*scale per glyph, 7*scale tall). */
int layer3_text_width(const char *str, int scale);

/* Rendering Buffer Access (the current band; 2 pixels per byte). */
const uint8_t* layer3_get_framebuffer(void);

#endif /* LAYER3_PRESENTATION_H */
