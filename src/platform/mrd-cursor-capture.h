#pragma once
#include <glib.h>
#include <stdint.h>
G_BEGIN_DECLS
typedef struct {
  uint8_t *bitmap;      
  uint16_t width;
  uint16_t height;
  uint16_t hotspot_x;
  uint16_t hotspot_y;
  uint32_t bitmap_size;
} MrdCursorInfo;

gboolean mrd_cursor_capture_get_current (MrdCursorInfo *info);

gboolean mrd_cursor_capture_compare (const MrdCursorInfo *a,
                                      const MrdCursorInfo *b);
G_END_DECLS