/**
 * @file lvgl.h
 * @brief LVGL (Light and Versatile Graphics Library) C99 Native Core API
 * 
 * Standard C99 LVGL Presentation Core for Embedded & HMI Applications
 */

#ifndef LVGL_H
#define LVGL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- LVGL Color Type --- */
typedef uint8_t lv_color_t;

/* --- LVGL Area Structure --- */
typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
} lv_area_t;

/* --- LVGL Object Declarations --- */
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

struct _lv_obj_t {
    struct _lv_obj_t * parent;
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    uint32_t flags;
    void * user_data;
};

struct _lv_group_t;
typedef struct _lv_group_t lv_group_t;

struct _lv_group_t {
    lv_obj_t * focus_obj;
    uint32_t obj_count;
    void * user_data;
};

struct _lv_disp_drv_t;
typedef struct _lv_disp_drv_t lv_disp_drv_t;

struct _lv_indev_drv_t;
typedef struct _lv_indev_drv_t lv_indev_drv_t;

/* --- Display Flush Callback Signature --- */
typedef void (*lv_disp_flush_cb_t)(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

/* --- Display Draw Buffer --- */
typedef struct {
    void * buf1;
    void * buf2;
    uint32_t size;
} lv_disp_draw_buf_t;

/* --- Display Driver Structure --- */
struct _lv_disp_drv_t {
    int16_t hor_res;
    int16_t ver_res;
    lv_disp_draw_buf_t * draw_buf;
    lv_disp_flush_cb_t flush_cb;
    void * user_data;
};

/* --- Input Device Types & Data --- */
typedef enum {
    LV_INDEV_TYPE_POINTER = 0,
    LV_INDEV_TYPE_KEYPAD,
    LV_INDEV_TYPE_BUTTON
} lv_indev_type_t;

typedef enum {
    LV_INDEV_STATE_RELEASED = 0,
    LV_INDEV_STATE_PRESSED
} lv_indev_state_t;

typedef struct {
    struct {
        int16_t x;
        int16_t y;
    } point;
    uint32_t key;
    lv_indev_state_t state;
} lv_indev_data_t;

typedef void (*lv_indev_read_cb_t)(lv_indev_drv_t * indev_drv, lv_indev_data_t * data);

struct _lv_indev_drv_t {
    lv_indev_type_t type;
    lv_indev_read_cb_t read_cb;
    lv_disp_drv_t * disp_drv;
    void * user_data;
};

typedef struct {
    lv_indev_drv_t * driver;
} lv_indev_t;

/* --- LVGL Object Alignment --- */
typedef enum {
    LV_ALIGN_TOP_LEFT = 0,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    LV_ALIGN_CENTER
} lv_align_t;

/* --- LVGL Event Types --- */
typedef enum {
    LV_EVENT_CLICKED = 0,
    LV_EVENT_VALUE_CHANGED,
    LV_EVENT_PRESSED,
    LV_EVENT_RELEASED,
    LV_EVENT_KEY
} lv_event_code_t;

typedef struct {
    lv_obj_t * target;
    lv_event_code_t code;
    void * param;
} lv_event_t;

typedef void (*lv_event_cb_t)(lv_event_t * e);

/* --- Core LVGL API Functions --- */
void lv_init(void);

/* Display Driver API */
void lv_disp_draw_buf_init(lv_disp_draw_buf_t * draw_buf, void * buf1, void * buf2, uint32_t size_in_px);
void lv_disp_drv_init(lv_disp_drv_t * driver);
struct _lv_disp_t * lv_disp_drv_register(lv_disp_drv_t * driver);
void lv_disp_flush_ready(lv_disp_drv_t * disp_drv);

/* Input Device Driver API */
void lv_indev_drv_init(lv_indev_drv_t * driver);
lv_indev_t * lv_indev_drv_register(lv_indev_drv_t * driver);
void lv_indev_set_group(lv_indev_t * indev, lv_group_t * group);

/* Group API */
lv_group_t * lv_group_create(void);
void lv_group_add_obj(lv_group_t * group, lv_obj_t * obj);
void lv_group_focus_next(lv_group_t * group);
void lv_group_focus_prev(lv_group_t * group);

/* Object API */
lv_obj_t * lv_obj_create(lv_obj_t * parent);
void lv_obj_set_size(lv_obj_t * obj, int16_t w, int16_t h);
void lv_obj_set_pos(lv_obj_t * obj, int16_t x, int16_t y);
void lv_obj_align(lv_obj_t * obj, lv_align_t align, int16_t x_ofs, int16_t y_ofs);
void lv_obj_add_event_cb(lv_obj_t * obj, lv_event_cb_t event_cb, lv_event_code_t filter, void * user_data);
void lv_obj_add_flag(lv_obj_t * obj, uint32_t f);
void lv_obj_clear_flag(lv_obj_t * obj, uint32_t f);

/* Widget Creators */
lv_obj_t * lv_label_create(lv_obj_t * parent);
void lv_label_set_text(lv_obj_t * label, const char * text);

lv_obj_t * lv_bar_create(lv_obj_t * parent);
void lv_bar_set_value(lv_obj_t * bar, int32_t value, bool anim);
void lv_bar_set_range(lv_obj_t * bar, int32_t min, int32_t max);

lv_obj_t * lv_btn_create(lv_obj_t * parent);

/* Chart API */
typedef enum {
    LV_CHART_TYPE_NONE = 0,
    LV_CHART_TYPE_LINE,
    LV_CHART_TYPE_BAR
} lv_chart_type_t;

typedef struct {
    lv_color_t color;
    int16_t * points;
} lv_chart_series_t;

lv_obj_t * lv_chart_create(lv_obj_t * parent);
void lv_chart_set_type(lv_obj_t * obj, lv_chart_type_t type);
lv_chart_series_t * lv_chart_add_series(lv_obj_t * chart, lv_color_t color, uint8_t axis);
void lv_chart_set_next_value(lv_obj_t * chart, lv_chart_series_t * ser, int16_t value);
void lv_chart_set_point_count(lv_obj_t * obj, uint16_t cnt);

/* Task / Timer Handler API */
uint32_t lv_task_handler(void);
void lv_tick_inc(uint32_t tick_period);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_H */
