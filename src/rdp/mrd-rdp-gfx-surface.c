#include "mrd-rdp-gfx-surface.h"
struct _MrdRdpGfxSurface
{
  GObject parent;
  uint16_t surface_id;
  uint32_t width;
  uint32_t height;
  gboolean mapped_to_output;
};
G_DEFINE_TYPE (MrdRdpGfxSurface, mrd_rdp_gfx_surface, G_TYPE_OBJECT)
MrdRdpGfxSurface *
mrd_rdp_gfx_surface_new (uint16_t surface_id,
                         uint32_t width,
                         uint32_t height)
{
  MrdRdpGfxSurface *surface;
  surface = g_object_new (MRD_TYPE_RDP_GFX_SURFACE, NULL);
  surface->surface_id = surface_id;
  surface->width = width;
  surface->height = height;
  return surface;
}
uint16_t
mrd_rdp_gfx_surface_get_id (MrdRdpGfxSurface *surface)
{
  g_return_val_if_fail (MRD_IS_RDP_GFX_SURFACE (surface), 0);
  return surface->surface_id;
}
uint32_t
mrd_rdp_gfx_surface_get_width (MrdRdpGfxSurface *surface)
{
  g_return_val_if_fail (MRD_IS_RDP_GFX_SURFACE (surface), 0);
  return surface->width;
}
uint32_t
mrd_rdp_gfx_surface_get_height (MrdRdpGfxSurface *surface)
{
  g_return_val_if_fail (MRD_IS_RDP_GFX_SURFACE (surface), 0);
  return surface->height;
}
void
mrd_rdp_gfx_surface_set_mapped (MrdRdpGfxSurface *surface,
                                gboolean          mapped)
{
  g_return_if_fail (MRD_IS_RDP_GFX_SURFACE (surface));
  surface->mapped_to_output = mapped;
}
gboolean
mrd_rdp_gfx_surface_get_mapped (MrdRdpGfxSurface *surface)
{
  g_return_val_if_fail (MRD_IS_RDP_GFX_SURFACE (surface), FALSE);
  return surface->mapped_to_output;
}
static void
mrd_rdp_gfx_surface_init (MrdRdpGfxSurface *surface)
{
}
static void
mrd_rdp_gfx_surface_class_init (MrdRdpGfxSurfaceClass *klass)
{
}