#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define ASSET_PACK_COLOR_ACCENT 0
#define ASSET_PACK_COLOR_BACKGROUND 1
#define ASSET_PACK_COLOR_SURFACE 2
#define ASSET_PACK_COLOR_SURFACE_ALT 3
#define ASSET_PACK_COLOR_TEXT 4
#define ASSET_PACK_COLOR_TEXT_MUTED 5
#define ASSET_PACK_INSTALLED_MAX 16

esp_err_t asset_pack_load_active(void);
esp_err_t asset_pack_extract_active_gtheme(void);
esp_err_t asset_pack_select_by_index(int index);
void asset_pack_switch_task(int index);
bool asset_pack_is_loaded(void);
uint32_t asset_pack_get_version(void);
const char *asset_pack_active_name(void);

int asset_pack_get_installed_count(void);
const char *asset_pack_get_installed_name(int index);
int asset_pack_get_current_index(void);
bool asset_pack_has_psram(void);

bool asset_pack_get_color(int slot, uint32_t *out_color);
const lv_img_dsc_t *asset_pack_get_icon(const char *name, const lv_img_dsc_t *fallback);
const lv_img_dsc_t *asset_pack_get_app_icon(const lv_img_dsc_t *fallback);

/* Returns a PSRAM-backed tiled background image, or NULL if unavailable. */
const lv_img_dsc_t *asset_pack_get_background_tile(void);
