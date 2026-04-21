#include "mrd-damage-utils.h"
#include <string.h>
bool
mrd_is_tile_dirty (cairo_rectangle_int_t *tile,
                   uint8_t               *current_data,
                   uint8_t               *prev_data,
                   uint32_t               stride,
                   uint32_t               bytes_per_pixel)
{
  uint32_t y;
  for (y = tile->y; y < tile->y + tile->height; ++y)
    {
      if (memcmp (prev_data + y * stride + tile->x * bytes_per_pixel,
                  current_data + y * stride + tile->x * bytes_per_pixel,
                  tile->width * bytes_per_pixel))
        return true;
    }
  return false;
}
cairo_region_t *
mrd_get_damage_region (uint8_t  *current_data,
                        uint8_t  *prev_data,
                        uint32_t  surface_width,
                        uint32_t  surface_height,
                        uint32_t  stride,
                        uint32_t  bytes_per_pixel)
{
  cairo_region_t *damage_region;
  cairo_rectangle_int_t tile;
  uint32_t cols, rows;
  uint32_t x, y;
  uint32_t tile_width = MRD_DAMAGE_TILE_WIDTH;
  uint32_t tile_height = MRD_DAMAGE_TILE_HEIGHT;
  damage_region = cairo_region_create ();
  
  if (current_data == NULL || prev_data == NULL)
    {
      tile.x = tile.y = 0;
      tile.width = surface_width;
      tile.height = surface_height;
      cairo_region_union_rectangle (damage_region, &tile);
      return damage_region;
    }
  cols = surface_width / tile_width + (surface_width % tile_width ? 1 : 0);
  rows = surface_height / tile_height + (surface_height % tile_height ? 1 : 0);
  for (y = 0; y < rows; ++y)
    {
      for (x = 0; x < cols; ++x)
        {
          tile.x = x * tile_width;
          tile.y = y * tile_height;
          tile.width = surface_width - tile.x < tile_width ? surface_width - tile.x
                                                           : tile_width;
          tile.height = surface_height - tile.y < tile_height ? surface_height - tile.y
                                                              : tile_height;
          if (mrd_is_tile_dirty (&tile, current_data, prev_data, stride, bytes_per_pixel))
            cairo_region_union_rectangle (damage_region, &tile);
        }
    }
  return damage_region;
}