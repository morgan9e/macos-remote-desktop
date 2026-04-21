#pragma once

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
  uint8_t *bitmap;      /* BGRA32, bottom-up (RDP format) */
  uint16_t width;
  uint16_t height;
  uint16_t hotspot_x;
  uint16_t hotspot_y;
  uint32_t bitmap_size;
} MrdCursorInfo;

/* Caller frees info->bitmap with g_free(). */
gboolean mrd_cursor_capture_get_current (MrdCursorInfo *info);

/* Cheap monotonic counter; -1 if private API unavailable. */
int mrd_cursor_capture_get_seed (void);

gboolean mrd_cursor_capture_compare (const MrdCursorInfo *a,
                                      const MrdCursorInfo *b);

G_END_DECLS
