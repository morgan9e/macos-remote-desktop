#pragma once
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#define MRD_DAMAGE_TILE_WIDTH  64
#define MRD_DAMAGE_TILE_HEIGHT 64

cairo_region_t *mrd_get_damage_region (uint8_t  *current_data,
                                        uint8_t  *prev_data,
                                        uint32_t  surface_width,
                                        uint32_t  surface_height,
                                        uint32_t  stride,
                                        uint32_t  bytes_per_pixel);

bool mrd_is_tile_dirty (cairo_rectangle_int_t *tile,
                        uint8_t               *current_data,
                        uint8_t               *prev_data,
                        uint32_t               stride,
                        uint32_t               bytes_per_pixel);