#pragma once

#include <stdbool.h>
#include <stdint.h>

#define THEME_PALETTE_THEME_COUNT 15
#define THEME_PALETTE_SLOT_COUNT 6

uint32_t theme_palette_get(uint8_t theme, int slot);
uint32_t theme_palette_get_accent(uint8_t theme);
bool theme_palette_is_bright(uint8_t theme);
