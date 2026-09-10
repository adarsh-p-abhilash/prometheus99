/**
 * @file layer3_presentation.h
 * @brief Layer 3: C99 Software Rasterizer & Pre-Allocated Static Screens
 * 
 * Implements widget rendering, unified input group handling,
 * 30 Hz refresh timer with dirty-rectangle optimization, and static screen memory.
 *
 * Changes from original:
 *   - Renamed from "LVGL Presentation" to "C99 Software Rasterizer" (LVGL not used)
 */

#ifndef LAYER3_PRESENTATION_H
#define LAYER3_PRESENTATION_H

#include "config.h"
#include "layer2_core.h"

/* --- High Contrast / Color Palette Theme --- */
typedef struct {
    uint32_t bg_color;
    uint32_t card_bg;
    uint32_t primary_accent;
    uint32_t secondary_accent;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t alarm_critical;
    uint32_t alarm_ok;
    bool is_high_contrast;
} hmi_theme_t;

/* --- Function Declarations --- */
void layer3_presentation_init(void);

/* 30 Hz UI Timer Tick Handler (renders, polls input, reports software alive) */
void layer3_ui_timer_tick_30hz(void);

/* Input Group Processing */
void layer3_inject_input_key(input_key_t key);
void layer3_handle_touch(int16_t x, int16_t y, bool pressed);

/* Theme & Contrast Toggle */
void layer3_toggle_high_contrast_theme(void);
const hmi_theme_t* layer3_get_current_theme(void);

/* Rendering Buffer Access */
const uint32_t* layer3_get_framebuffer(void);

#endif /* LAYER3_PRESENTATION_H */
