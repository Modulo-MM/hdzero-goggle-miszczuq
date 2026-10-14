#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include <lvgl/lvgl.h>

// LVGL setters that do nothing when the object already shows the value
// (perf_opt.lazy_ui; with it off they are the plain LVGL calls).
//
// Every LVGL setter invalidates its object even when nothing changes, and
// lv_img_set_src() of a file re-reads the file's header too. The main loop
// re-applies the OSD and status bar state every 5 ms, so without this the
// display is redrawn continuously -- the whole screen, with full_refresh.
void lazy_label_set_text(lv_obj_t *obj, const char *text);
void lazy_img_set_src(lv_obj_t *obj, const void *src);
void lazy_hidden(lv_obj_t *obj, bool hidden);
void lazy_text_color(lv_obj_t *obj, lv_color_t color); // selector 0, as the callers use
void lazy_bg_opa(lv_obj_t *obj, lv_opa_t opa);         // selector 0

#ifdef __cplusplus
}
#endif
