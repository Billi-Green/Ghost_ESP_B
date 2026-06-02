#include "gui/screen_layout.h"
#include "gui/asset_pack.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"

lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa) {
    if (!parent) parent = lv_scr_act();

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_color(root, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);

    const lv_img_dsc_t *bg_tile = asset_pack_get_background_tile();
    if (bg_tile) {
        lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_t *bg_img = lv_img_create(root);
        lv_img_set_src(bg_img, bg_tile);
        lv_img_set_antialias(bg_img, false);

        lv_coord_t img_w = bg_tile->header.w;
        lv_coord_t img_h = bg_tile->header.h;
        bool tile = (img_w < LV_HOR_RES || img_h < LV_VER_RES);

        if (tile) {
            lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_style_bg_img_src(bg_img, bg_tile, LV_PART_MAIN);
            lv_obj_set_style_bg_img_tiled(bg_img, true, LV_PART_MAIN);
            lv_obj_set_style_bg_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bg_img, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(bg_img, 0, LV_PART_MAIN);
            lv_obj_set_style_img_opa(bg_img, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_img_set_zoom(bg_img, 256);
        } else {
            lv_obj_set_style_bg_img_src(bg_img, NULL, LV_PART_MAIN);
            lv_obj_set_style_bg_img_tiled(bg_img, false, LV_PART_MAIN);
            int zoom_w = (img_w > 0) ? (LV_HOR_RES * 256) / img_w : 256;
            int zoom_h = (img_h > 0) ? (LV_VER_RES * 256) / img_h : 256;
            int zoom = (zoom_w > zoom_h) ? zoom_w : zoom_h;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(bg_img, zoom);
            lv_img_set_pivot(bg_img, 0, 0);
            lv_obj_set_pos(bg_img, 0, 0);
            lv_obj_set_style_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
        }
        lv_obj_move_to_index(bg_img, 0);
    }

    if (title && title[0]) {
        display_manager_add_status_bar(title);
    }

    return root;
}

void gui_screen_apply_background(lv_obj_t *root) {
    if (!root || !lv_obj_is_valid(root)) return;

    lv_obj_t *existing_bg = NULL;
    for (int i = 0; i < (int)lv_obj_get_child_cnt(root); i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child && lv_obj_check_type(child, &lv_img_class)) {
            existing_bg = child;
            break;
        }
    }

    const lv_img_dsc_t *bg_tile = asset_pack_get_background_tile();
    if (!bg_tile) {
        if (existing_bg) lv_obj_del(existing_bg);
        return;
    }
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    if (!existing_bg) {
        existing_bg = lv_img_create(root);
        lv_obj_move_to_index(existing_bg, 0);
    }

    lv_img_set_src(existing_bg, bg_tile);
    lv_img_set_antialias(existing_bg, false);

    lv_coord_t img_w = bg_tile->header.w;
    lv_coord_t img_h = bg_tile->header.h;
    bool tile = (img_w < LV_HOR_RES || img_h < LV_VER_RES);

    if (tile) {
        lv_obj_set_size(existing_bg, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_img_src(existing_bg, bg_tile, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(existing_bg, true, LV_PART_MAIN);
        lv_obj_set_style_bg_img_opa(existing_bg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(existing_bg, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(existing_bg, 0, LV_PART_MAIN);
        lv_obj_set_style_img_opa(existing_bg, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_img_set_zoom(existing_bg, 256);
    } else {
        lv_obj_set_style_bg_img_src(existing_bg, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(existing_bg, false, LV_PART_MAIN);
        int zoom_w = (img_w > 0) ? (LV_HOR_RES * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (LV_VER_RES * 256) / img_h : 256;
        int zoom = (zoom_w > zoom_h) ? zoom_w : zoom_h;
        if (zoom < 64) zoom = 64;
        lv_img_set_zoom(existing_bg, zoom);
        lv_img_set_pivot(existing_bg, 0, 0);
        lv_obj_set_pos(existing_bg, 0, 0);
        lv_obj_set_style_img_opa(existing_bg, LV_OPA_COVER, LV_PART_MAIN);
    }
}

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h) {
    if (!root) return NULL;

    if (status_bar_h < 0) status_bar_h = 0;
    if (status_bar_h > LV_VER_RES) status_bar_h = LV_VER_RES;

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_pos(content, 0, status_bar_h);
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - status_bar_h);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);

    return content;
}
