#pragma once

#include <glib-object.h>
#include <stdint.h>

G_BEGIN_DECLS

#define MRD_TYPE_VIRTUAL_DISPLAY (mrd_virtual_display_get_type ())
G_DECLARE_FINAL_TYPE (MrdVirtualDisplay, mrd_virtual_display,
                      MRD, VIRTUAL_DISPLAY, GObject)

/* 1x scale (no HiDPI). */
MrdVirtualDisplay *mrd_virtual_display_new (uint32_t  width,
                                            uint32_t  height,
                                            uint32_t  refresh_hz,
                                            GError  **error);

/* Backing is 2x (width*2 × height*2). */
MrdVirtualDisplay *mrd_virtual_display_new_hidpi (uint32_t  width,
                                                   uint32_t  height,
                                                   uint32_t  refresh_hz,
                                                   GError  **error);

MrdVirtualDisplay *mrd_virtual_display_new_scaled (uint32_t  physical_width,
                                                    uint32_t  physical_height,
                                                    uint32_t  scale_percent,
                                                    uint32_t  refresh_hz,
                                                    GError  **error);

/* CGDirectDisplayID; 0 if inactive. */
uint32_t mrd_virtual_display_get_id (MrdVirtualDisplay *vd);

gboolean mrd_virtual_display_is_hidpi (MrdVirtualDisplay *vd);

void mrd_virtual_display_get_logical_size (MrdVirtualDisplay *vd,
                                            uint32_t          *width,
                                            uint32_t          *height);

/* Returns FALSE (caller falls back to recreate) for HiDPI toggle or
 * backing-size > descriptor envelope. */
gboolean mrd_virtual_display_reconfigure (MrdVirtualDisplay *vd,
                                          uint32_t           logical_width,
                                          uint32_t           logical_height,
                                          uint32_t           refresh_hz,
                                          gboolean           hidpi,
                                          GError           **error);

/* Restored on VD destroy. */
gboolean mrd_virtual_display_make_primary (MrdVirtualDisplay *vd);
gboolean mrd_virtual_display_mirror_physical (MrdVirtualDisplay *vd);

typedef enum {
  MRD_VD_SIDE_LEFT,
  MRD_VD_SIDE_RIGHT,
} MrdVdSide;

/* macOS persists arrangement across sessions; this overrides any stale
 * position so the extended VD lands flush against primary's left/right edge. */
gboolean mrd_virtual_display_place_extended (MrdVirtualDisplay *vd,
                                             MrdVdSide          side);

G_END_DECLS
