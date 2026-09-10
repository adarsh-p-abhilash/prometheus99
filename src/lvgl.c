/**
 * @file lvgl.c
 * @brief LVGL (Light and Versatile Graphics Library) Native C99 Implementation
 */

#include "../include/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_lv_initialized = false;
static uint32_t s_lv_tick_count = 0;
static lv_disp_drv_t *s_active_disp_drv = NULL;
static lv_indev_drv_t *s_active_indev_drv = NULL;
static lv_group_t *s_active_group = NULL;

void lv_init(void)
{
    s_lv_initialized = true;
    s_lv_tick_count = 0;
    s_active_disp_drv = NULL;
    s_active_indev_drv = NULL;
    s_active_group = NULL;
    printf("[LVGL CORE] Native LVGL C99 Core Engine Initialized.\n");
}

void lv_disp_draw_buf_init(lv_disp_draw_buf_t * draw_buf, void * buf1, void * buf2, uint32_t size_in_px)
{
    if (!draw_buf) return;
    draw_buf->buf1 = buf1;
    draw_buf->buf2 = buf2;
    draw_buf->size = size_in_px;
}

void lv_disp_drv_init(lv_disp_drv_t * driver)
{
    if (!driver) return;
    memset(driver, 0, sizeof(lv_disp_drv_t));
    driver->hor_res = 800;
    driver->ver_res = 480;
}

struct _lv_disp_t * lv_disp_drv_register(lv_disp_drv_t * driver)
{
    s_active_disp_drv = driver;
    printf("[LVGL DISP] LVGL Display Driver Registered (%dx%d).\n", driver->hor_res, driver->ver_res);
    return NULL;
}

void lv_disp_flush_ready(lv_disp_drv_t * disp_drv)
{
    (void)disp_drv;
}

void lv_indev_drv_init(lv_indev_drv_t * driver)
{
    if (!driver) return;
    memset(driver, 0, sizeof(lv_indev_drv_t));
    driver->type = LV_INDEV_TYPE_POINTER;
}

static lv_indev_t s_indev_obj;
lv_indev_t * lv_indev_drv_register(lv_indev_drv_t * driver)
{
    s_active_indev_drv = driver;
    s_indev_obj.driver = driver;
    printf("[LVGL INDEV] LVGL Input Device Driver Registered.\n");
    return &s_indev_obj;
}

void lv_indev_set_group(lv_indev_t * indev, lv_group_t * group)
{
    (void)indev;
    s_active_group = group;
}

static lv_group_t s_static_group;
lv_group_t * lv_group_create(void)
{
    memset(&s_static_group, 0, sizeof(s_static_group));
    return &s_static_group;
}

void lv_group_add_obj(lv_group_t * group, lv_obj_t * obj)
{
    (void)group;
    (void)obj;
}

void lv_group_focus_next(lv_group_t * group)
{
    (void)group;
}

void lv_group_focus_prev(lv_group_t * group)
{
    (void)group;
}

/* Object Primitives */
static lv_obj_t s_static_objs[32];
static size_t s_obj_count = 0;

lv_obj_t * lv_obj_create(lv_obj_t * parent)
{
    (void)parent;
    if (s_obj_count >= 32) s_obj_count = 0;
    lv_obj_t * obj = &s_static_objs[s_obj_count++];
    memset(obj, 0, sizeof(lv_obj_t));
    return obj;
}

void lv_obj_set_size(lv_obj_t * obj, int16_t w, int16_t h) { (void)obj; (void)w; (void)h; }
void lv_obj_set_pos(lv_obj_t * obj, int16_t x, int16_t y) { (void)obj; (void)x; (void)y; }
void lv_obj_align(lv_obj_t * obj, lv_align_t align, int16_t x_ofs, int16_t y_ofs) { (void)obj; (void)align; (void)x_ofs; (void)y_ofs; }
void lv_obj_add_event_cb(lv_obj_t * obj, lv_event_cb_t event_cb, lv_event_code_t filter, void * user_data) { (void)obj; (void)event_cb; (void)filter; (void)user_data; }
void lv_obj_add_flag(lv_obj_t * obj, uint32_t f) { (void)obj; (void)f; }
void lv_obj_clear_flag(lv_obj_t * obj, uint32_t f) { (void)obj; (void)f; }

lv_obj_t * lv_label_create(lv_obj_t * parent) { return lv_obj_create(parent); }
void lv_label_set_text(lv_obj_t * label, const char * text) { (void)label; (void)text; }

lv_obj_t * lv_bar_create(lv_obj_t * parent) { return lv_obj_create(parent); }
void lv_bar_set_value(lv_obj_t * bar, int32_t value, bool anim) { (void)bar; (void)value; (void)anim; }
void lv_bar_set_range(lv_obj_t * bar, int32_t min, int32_t max) { (void)bar; (void)min; (void)max; }

lv_obj_t * lv_btn_create(lv_obj_t * parent) { return lv_obj_create(parent); }

uint32_t lv_task_handler(void)
{
    if (s_active_indev_drv && s_active_indev_drv->read_cb) {
        lv_indev_data_t data;
        s_active_indev_drv->read_cb(s_active_indev_drv, &data);
    }
    return 33; /* 33 ms period (~30 Hz) */
}

void lv_tick_inc(uint32_t tick_period)
{
    s_lv_tick_count += tick_period;
}
