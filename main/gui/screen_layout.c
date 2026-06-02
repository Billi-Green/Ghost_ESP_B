#include "gui/screen_layout.h"
#include "gui/asset_pack.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"

/* Tracks the last asset-pack version for which we built a bg widget on a
 * given root, so gui_screen_apply_background() can early-out when called
 * repeatedly (e.g. from the 1Hz menu refresh timer) without doing work. */
static uint32_t s_last_applied_bg_version = 0xFFFFFFFFu;
static lv_obj_t *s_last_applied_bg_root = NULL;
static const lv_img_dsc_t *s_last_applied_bg_src = NULL;
static lv_obj_t *s_last_applied_bg_widget = NULL;

/* Drop the short-circuit cache (e.g. after lv_obj_clean on the root). */
static void invalidate_bg_shortcut(void) {
    s_last_applied_bg_root = NULL;
    s_last_applied_bg_widget = NULL;
    s_last_applied_bg_src = NULL;
    s_last_applied_bg_version = 0xFFFFFFFFu;
}

static void apply_bg_widget(lv_obj_t *root, const lv_img_dsc_t *src) {
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *bg_img = NULL;
    for (int i = 0; i < (int)lv_obj_get_child_cnt(root); i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child && lv_obj_check_type(child, &lv_img_class)) {
            bg_img = child;
            break;
        }
    }
    if (!bg_img) {
        bg_img = lv_img_create(root);
        lv_obj_move_to_index(bg_img, 0);
    }

    s_last_applied_bg_widget = bg_img;
    lv_img_set_src(bg_img, src);
    lv_img_set_antialias(bg_img, false);

    /* If src is the pre-baked fullscreen desc, it already matches the
     * display exactly. No zoom, no tiling style, just a flat blit. */
    if (src->header.w == (uint16_t)LV_HOR_RES && src->header.h == (uint16_t)LV_VER_RES) {
        lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_img_src(bg_img, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_img_tiled(bg_img, false, LV_PART_MAIN);
        lv_obj_set_style_img_opa(bg_img, LV_OPA_COVER, LV_PART_MAIN);
        lv_img_set_zoom(bg_img, 256);
        lv_obj_set_pos(bg_img, 0, 0);
        return;
    }

    lv_coord_t img_w = src->header.w;
    lv_coord_t img_h = src->header.h;
    bool tile = (img_w < LV_HOR_RES || img_h < LV_VER_RES);

    if (tile) {
        lv_obj_set_size(bg_img, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_img_src(bg_img, src, LV_PART_MAIN);
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
}

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

    /* Prefer the pre-baked fullscreen bg (single blit) over the smaller tile. */
    const lv_img_dsc_t *bg_src = asset_pack_get_background_fullscreen();
    if (!bg_src) bg_src = asset_pack_get_background_tile();
    if (bg_src) {
        apply_bg_widget(root, bg_src);
        s_last_applied_bg_root = root;
        s_last_applied_bg_src = bg_src;
        s_last_applied_bg_version = asset_pack_get_version();
    }

    if (title && title[0]) {
        display_manager_add_status_bar(title);
    }

    return root;
}

void gui_screen_apply_background(lv_obj_t *root) {
    if (!root || !lv_obj_is_valid(root)) return;

    /* Fast path: if the bg widget on this root already shows the current
     * pack's image, do nothing. This skips a 150KB re-blit when the 1Hz
     * menu refresh timer or any other periodic caller invokes us. The
     * widget-presence check is required because callers like the 1Hz
     * menu rebuild do lv_obj_clean(root) which destroys the bg widget
     * even though the version didn't change. */
    uint32_t cur_ver = asset_pack_get_version();
    const lv_img_dsc_t *expected = asset_pack_get_background_fullscreen();
    if (!expected) expected = asset_pack_get_background_tile();
    if (s_last_applied_bg_root == root && s_last_applied_bg_version == cur_ver &&
        s_last_applied_bg_widget && lv_obj_is_valid(s_last_applied_bg_widget) &&
        expected == s_last_applied_bg_src) {
        return;
    }

    const lv_img_dsc_t *bg_src = asset_pack_get_background_fullscreen();
    if (!bg_src) bg_src = asset_pack_get_background_tile();
    if (!bg_src) {
        /* No background available - drop any existing bg widget. */
        for (int i = 0; i < (int)lv_obj_get_child_cnt(root); i++) {
            lv_obj_t *child = lv_obj_get_child(root, i);
            if (child && lv_obj_check_type(child, &lv_img_class)) {
                lv_obj_del(child);
                break;
            }
        }
        s_last_applied_bg_root = NULL;
        s_last_applied_bg_src = NULL;
        s_last_applied_bg_version = cur_ver;
        return;
    }

    apply_bg_widget(root, bg_src);
    s_last_applied_bg_root = root;
    s_last_applied_bg_src = bg_src;
    s_last_applied_bg_version = cur_ver;
}

void gui_screen_invalidate_bg_cache(void) {
    /* Call after lv_obj_clean or destroy on a root that had a cached bg. */
    invalidate_bg_shortcut();
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
