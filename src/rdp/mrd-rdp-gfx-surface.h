#pragma once
#include <glib-object.h>
#include <stdint.h>
#include "../mrd-types.h"
G_BEGIN_DECLS
#define MRD_TYPE_RDP_GFX_SURFACE (mrd_rdp_gfx_surface_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpGfxSurface, mrd_rdp_gfx_surface, MRD, RDP_GFX_SURFACE, GObject)
MrdRdpGfxSurface *mrd_rdp_gfx_surface_new (uint16_t surface_id,
                                           uint32_t width,
                                           uint32_t height);
uint16_t mrd_rdp_gfx_surface_get_id (MrdRdpGfxSurface *surface);
uint32_t mrd_rdp_gfx_surface_get_width (MrdRdpGfxSurface *surface);
uint32_t mrd_rdp_gfx_surface_get_height (MrdRdpGfxSurface *surface);
void mrd_rdp_gfx_surface_set_mapped (MrdRdpGfxSurface *surface,
                                     gboolean          mapped);
gboolean mrd_rdp_gfx_surface_get_mapped (MrdRdpGfxSurface *surface);
G_END_DECLS