#include "ui/ui_lazy.h"

#include <string.h>

#include "core/perf_monitor.h"

void lazy_label_set_text(lv_obj_t *obj, const char *text) {
    if (perf_opt.lazy_ui && text) {
        const char *cur = lv_label_get_text(obj);
        if (cur && strcmp(cur, text) == 0)
            return;
    }
    lv_label_set_text(obj, text);
}

void lazy_img_set_src(lv_obj_t *obj, const void *src) {
    if (perf_opt.lazy_ui && src) {
        const void *cur = lv_img_get_src(obj);
        lv_img_src_t type = lv_img_src_get_type(src);
        if (cur && type == lv_img_src_get_type(cur)) {
            if (type == LV_IMG_SRC_VARIABLE && cur == src)
                return;
            // lv_img keeps its own copy of a path or symbol string
            if ((type == LV_IMG_SRC_FILE || type == LV_IMG_SRC_SYMBOL) && strcmp(cur, src) == 0)
                return;
        }
    }
    lv_img_set_src(obj, src);
}

void lazy_hidden(lv_obj_t *obj, bool hidden) {
    if (perf_opt.lazy_ui && lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden)
        return;
    if (hidden)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void lazy_text_color(lv_obj_t *obj, lv_color_t color) {
    if (perf_opt.lazy_ui) {
        lv_style_value_t v;
        if (lv_obj_get_local_style_prop(obj, LV_STYLE_TEXT_COLOR, &v, 0) == LV_STYLE_RES_FOUND &&
            v.color.full == color.full)
            return;
    }
    lv_obj_set_style_text_color(obj, color, 0);
}

void lazy_bg_opa(lv_obj_t *obj, lv_opa_t opa) {
    if (perf_opt.lazy_ui) {
        lv_style_value_t v;
        if (lv_obj_get_local_style_prop(obj, LV_STYLE_BG_OPA, &v, 0) == LV_STYLE_RES_FOUND && v.num == opa)
            return;
    }
    lv_obj_set_style_bg_opa(obj, opa, 0);
}
