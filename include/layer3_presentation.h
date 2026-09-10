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

/* --- High Contrast / Color Palette Theme --- */
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
void layer3_presentation_init(void);

/* 30 Hz UI Timer Tick Handler (Executes rendering, polls dirty flag, reports software alive) */
void layer3_ui_timer_tick_30hz(void);

/* Input Group Processing */
void layer3_inject_input_key(input_key_t key);
void layer3_handle_touch(int16_t x, int16_t y, bool pressed);

/* Theme & Contrast Toggle */
void layer3_toggle_high_contrast_theme(void);
const hmi_theme_t* layer3_get_current_theme(void);

/* Rendering Buffer Access */
const uint8_t* layer3_get_framebuffer(void);

#endif /* LAYER3_PRESENTATION_H */
