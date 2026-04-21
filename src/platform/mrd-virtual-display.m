#include "mrd-virtual-display.h"
#include "CGVirtualDisplayPrivate.h"
#import <CoreGraphics/CoreGraphics.h>
#include <gio/gio.h>
#include <unistd.h>
struct _MrdVirtualDisplay
{
  GObject parent;
  CGVirtualDisplay *display;
  uint32_t display_id;
  gboolean is_hidpi;
  uint32_t logical_width;
  uint32_t logical_height;
  
  CGDirectDisplayID prev_main_display;
  CGPoint prev_main_origin;
  gboolean was_made_primary;
  gboolean was_mirrored;
};
G_DEFINE_TYPE (MrdVirtualDisplay, mrd_virtual_display, G_TYPE_OBJECT)

static uint32_t g_vd_session_counter = 0;
static MrdVirtualDisplay *
create_virtual_display (uint32_t  logical_width,
                        uint32_t  logical_height,
                        uint32_t  refresh_hz,
                        gboolean  hidpi,
                        GError  **error)
{
  MrdVirtualDisplay *self = g_object_new (MRD_TYPE_VIRTUAL_DISPLAY, NULL);
  
  uint32_t max_pixels_wide = hidpi ? logical_width * 2 : logical_width;
  uint32_t max_pixels_high = hidpi ? logical_height * 2 : logical_height;
  
  uint32_t serial = ((uint32_t)getpid () << 16) | (++g_vd_session_counter & 0xFFFF);
  @autoreleasepool {
    CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
    desc.name = @"Remote Display";
    desc.maxPixelsWide = max_pixels_wide;
    desc.maxPixelsHigh = max_pixels_high;
    desc.sizeInMillimeters = CGSizeMake (logical_width / 100.0 * 25.4,
                                         logical_height / 100.0 * 25.4);
    
    desc.vendorID  = 0x4D52;  
    desc.productID = 0x4450;  
    desc.serialNum = serial;
    desc.terminationHandler = ^(id __unused userInfo, CGVirtualDisplay __unused *d) {};
    dispatch_queue_t queue = dispatch_queue_create ("mrd.virtualdisplay",
                                                    DISPATCH_QUEUE_SERIAL);
    [desc setDispatchQueue:queue];
    self->display = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
    if (!self->display)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "CGVirtualDisplay alloc failed. Check: (1) binary is codesigned "
                     "(codesign --force --sign - ...); (2) no other VD uses the same "
                     "(vendor,product,serial) tuple; (3) dispatch queue set on descriptor.");
        g_object_unref (self);
        return NULL;
      }
    CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
    settings.hiDPI = hidpi ? 1 : 0;
    settings.modes = @[
      [[CGVirtualDisplayMode alloc] initWithWidth:logical_width
                                           height:logical_height
                                      refreshRate:(CGFloat)refresh_hz]
    ];
    if (![self->display applySettings:settings])
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "CGVirtualDisplay applySettings failed");
        g_object_unref (self);
        return NULL;
      }
    self->display_id = (uint32_t) self->display.displayID;
    self->is_hidpi = hidpi;
    self->logical_width = logical_width;
    self->logical_height = logical_height;
    if (hidpi)
      g_message ("Virtual display created: id=%u  %ux%u logical (HiDPI %ux%u backing) @ %uHz",
                 self->display_id, logical_width, logical_height,
                 max_pixels_wide, max_pixels_high, refresh_hz);
    else
      g_message ("Virtual display created: id=%u  %ux%u @ %uHz",
                 self->display_id, logical_width, logical_height, refresh_hz);
  }
  return self;
}
MrdVirtualDisplay *
mrd_virtual_display_new (uint32_t  width,
                         uint32_t  height,
                         uint32_t  refresh_hz,
                         GError  **error)
{
  return create_virtual_display (width, height, refresh_hz, FALSE, error);
}
MrdVirtualDisplay *
mrd_virtual_display_new_hidpi (uint32_t  width,
                                uint32_t  height,
                                uint32_t  refresh_hz,
                                GError  **error)
{
  return create_virtual_display (width, height, refresh_hz, TRUE, error);
}
MrdVirtualDisplay *
mrd_virtual_display_new_scaled (uint32_t  physical_width,
                                 uint32_t  physical_height,
                                 uint32_t  scale_percent,
                                 uint32_t  refresh_hz,
                                 GError  **error)
{
  
  uint32_t logical_width = (physical_width * 100) / scale_percent;
  uint32_t logical_height = (physical_height * 100) / scale_percent;
  
  logical_width = (logical_width + 1) & ~1;
  logical_height = (logical_height + 1) & ~1;
  g_message ("Scaled VD: physical %ux%u at %u%% → logical %ux%u (HiDPI)",
             physical_width, physical_height, scale_percent,
             logical_width, logical_height);
  return create_virtual_display (logical_width, logical_height, refresh_hz, TRUE, error);
}
uint32_t
mrd_virtual_display_get_id (MrdVirtualDisplay *self)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), 0);
  return self->display_id;
}
gboolean
mrd_virtual_display_is_hidpi (MrdVirtualDisplay *self)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), FALSE);
  return self->is_hidpi;
}
void
mrd_virtual_display_get_logical_size (MrdVirtualDisplay *self,
                                       uint32_t          *width,
                                       uint32_t          *height)
{
  g_return_if_fail (MRD_IS_VIRTUAL_DISPLAY (self));
  if (width)
    *width = self->logical_width;
  if (height)
    *height = self->logical_height;
}
gboolean
mrd_virtual_display_make_primary (MrdVirtualDisplay *self)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), FALSE);
  if (self->display_id == 0)
    return FALSE;
  
  for (int attempt = 0; attempt < 10; attempt++)
    {
      CGRect vd_bounds = CGDisplayBounds (self->display_id);
      if (!CGRectIsNull (vd_bounds) && !CGRectIsEmpty (vd_bounds))
        break;
      g_usleep (100000);  
    }
  CGDirectDisplayID old_main = CGMainDisplayID ();
  if (old_main == self->display_id)
    {
      self->was_made_primary = TRUE;
      return TRUE;
    }
  
  CGDirectDisplayID displays[16];
  uint32_t count = 0;
  if (CGGetOnlineDisplayList (16, displays, &count) != kCGErrorSuccess)
    count = 0;
  CGRect old_bounds = CGDisplayBounds (old_main);
  
  CGDisplayConfigRef config;
  if (CGBeginDisplayConfiguration (&config) != kCGErrorSuccess)
    {
      g_warning ("CGBeginDisplayConfiguration failed");
      return FALSE;
    }
  CGConfigureDisplayOrigin (config, self->display_id, 0, 0);
  CGConfigureDisplayOrigin (config, old_main,
                            (int32_t)self->logical_width, 0);
  CGError err = CGCompleteDisplayConfiguration (config, kCGConfigureForSession);
  if (err != kCGErrorSuccess)
    {
      g_warning ("make_primary failed (error %d)", err);
      CGCancelDisplayConfiguration (config);
      return FALSE;
    }
  self->prev_main_display = old_main;
  self->prev_main_origin = old_bounds.origin;
  self->was_made_primary = TRUE;
  g_message ("VD %u set as primary (previous: %u)", self->display_id, old_main);
  
  uint32_t mirrored = 0;
  if (count > 1)
    {
      if (CGBeginDisplayConfiguration (&config) == kCGErrorSuccess)
        {
          for (uint32_t i = 0; i < count; i++)
            {
              if (displays[i] == self->display_id)
                continue;
              CGConfigureDisplayMirrorOfDisplay (config, displays[i], self->display_id);
              mirrored++;
            }
          err = CGCompleteDisplayConfiguration (config, kCGConfigureForSession);
          if (err != kCGErrorSuccess)
            {
              g_warning ("Display mirroring failed (error %d) — VD is primary but not mirrored", err);
              CGCancelDisplayConfiguration (config);
              mirrored = 0;
            }
        }
    }
  self->was_mirrored = (mirrored > 0);
  if (mirrored > 0)
    g_message ("Mirrored %u display(s) to VD %u", mirrored, self->display_id);
  return TRUE;
}
gboolean
mrd_virtual_display_mirror_physical (MrdVirtualDisplay *self)
{
  
  return self->was_mirrored;
}
static void
restore_display_layout (MrdVirtualDisplay *self)
{
  if (!self->was_mirrored && !self->was_made_primary)
    return;
  CGDisplayConfigRef config;
  if (CGBeginDisplayConfiguration (&config) != kCGErrorSuccess)
    return;
  
  if (self->was_mirrored)
    {
      CGDirectDisplayID displays[16];
      uint32_t count = 0;
      CGGetOnlineDisplayList (16, displays, &count);
      for (uint32_t i = 0; i < count; i++)
        {
          if (displays[i] == self->display_id)
            continue;
          
          CGConfigureDisplayMirrorOfDisplay (config, displays[i],
                                             kCGNullDirectDisplay);
        }
    }
  
  if (self->was_made_primary)
    CGConfigureDisplayOrigin (config, self->prev_main_display, 0, 0);
  CGCompleteDisplayConfiguration (config, kCGConfigureForSession);
  g_message ("Restored display layout (primary: %u)", self->prev_main_display);
}
static void
mrd_virtual_display_finalize (GObject *object)
{
  MrdVirtualDisplay *self = MRD_VIRTUAL_DISPLAY (object);
  restore_display_layout (self);
  if (self->display)
    {
      g_message ("Virtual display destroyed: id=%u", self->display_id);
      self->display = nil;
    }
  G_OBJECT_CLASS (mrd_virtual_display_parent_class)->finalize (object);
}
static void
mrd_virtual_display_init (MrdVirtualDisplay *self) {}
static void
mrd_virtual_display_class_init (MrdVirtualDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_virtual_display_finalize;
}