#pragma once
#include <glib-object.h>
#include <stdint.h>
G_BEGIN_DECLS
#define MRD_TYPE_VIRTUAL_DISPLAY (mrd_virtual_display_get_type ())
G_DECLARE_FINAL_TYPE (MrdVirtualDisplay, mrd_virtual_display,
                      MRD, VIRTUAL_DISPLAY, GObject)

MrdVirtualDisplay *mrd_virtual_display_new (uint32_t  width,
                                            uint32_t  height,
                                            uint32_t  refresh_hz,
                                            GError  **error);

MrdVirtualDisplay *mrd_virtual_display_new_hidpi (uint32_t  width,
                                                   uint32_t  height,
                                                   uint32_t  refresh_hz,
                                                   GError  **error);
MrdVirtualDisplay *mrd_virtual_display_new_scaled (uint32_t  physical_width,
                                                    uint32_t  physical_height,
                                                    uint32_t  scale_percent,
                                                    uint32_t  refresh_hz,
                                                    GError  **error);

uint32_t mrd_virtual_display_get_id (MrdVirtualDisplay *vd);

gboolean mrd_virtual_display_is_hidpi (MrdVirtualDisplay *vd);

void mrd_virtual_display_get_logical_size (MrdVirtualDisplay *vd,
                                            uint32_t          *width,
                                            uint32_t          *height);

gboolean mrd_virtual_display_make_primary (MrdVirtualDisplay *vd);

gboolean mrd_virtual_display_mirror_physical (MrdVirtualDisplay *vd);
G_END_DECLS