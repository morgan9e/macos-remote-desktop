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
  uint32_t refresh_hz;

  /* Backing envelope; reconfigure() must stay within it. */
  uint32_t max_pixels_wide;
  uint32_t max_pixels_high;

  CGDirectDisplayID prev_main_display;
  CGPoint prev_main_origin;
  gboolean was_made_primary;
  gboolean was_mirrored;
};

G_DEFINE_TYPE (MrdVirtualDisplay, mrd_virtual_display, G_TYPE_OBJECT)

/* Stable (vendor, product, serial) so macOS treats every VD as the SAME
 * display across sessions — distinct serials accumulate ghost entries in
 * com.apple.windowserver.displays.plist and ColorSync. */
#define MRD_VD_SERIAL 1

/* 4K HiDPI envelope so any mode change fits applySettings: in place
 * (keeps displayID + placement stable). MRD_REPLACE_VD=1 disables it. */
#define MRD_VD_FIXED_ENVELOPE_W  7680
#define MRD_VD_FIXED_ENVELOPE_H  4320

static MrdVirtualDisplay *
create_virtual_display (uint32_t  logical_width,
                        uint32_t  logical_height,
                        uint32_t  refresh_hz,
                        gboolean  hidpi,
                        GError  **error)
{
  MrdVirtualDisplay *self = g_object_new (MRD_TYPE_VIRTUAL_DISPLAY, NULL);

  uint32_t base_w = hidpi ? logical_width * 2 : logical_width;
  uint32_t base_h = hidpi ? logical_height * 2 : logical_height;

  const char *replace_env = g_getenv ("MRD_REPLACE_VD");
  gboolean force_replace = replace_env && replace_env[0] != '\0' &&
                           g_strcmp0 (replace_env, "0") != 0;
  uint32_t max_pixels_wide = force_replace
    ? base_w : MAX (base_w, (uint32_t) MRD_VD_FIXED_ENVELOPE_W);
  uint32_t max_pixels_high = force_replace
    ? base_h : MAX (base_h, (uint32_t) MRD_VD_FIXED_ENVELOPE_H);

  uint32_t serial = MRD_VD_SERIAL;

  @autoreleasepool {
    CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
    desc.name = @"Remote Display";
    desc.maxPixelsWide = max_pixels_wide;
    desc.maxPixelsHigh = max_pixels_high;
    desc.sizeInMillimeters = CGSizeMake (logical_width / 100.0 * 25.4,
                                         logical_height / 100.0 * 25.4);
    /* Distinct from Examples/VirtualDisplay sample so both can coexist. */
    desc.vendorID  = 0x4D52;  /* 'MR' */
    desc.productID = 0x4450;  /* 'DP' */
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
                                      refreshRate:(CGFloat) refresh_hz]
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
    self->refresh_hz = refresh_hz;
    self->max_pixels_wide = max_pixels_wide;
    self->max_pixels_high = max_pixels_high;

    g_message ("Virtual display created: id=%u  %ux%u%s @ %uHz",
               self->display_id, logical_width, logical_height,
               hidpi ? " (HiDPI)" : "", refresh_hz);
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

  /* Even dimensions required by codec. */
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
mrd_virtual_display_reconfigure (MrdVirtualDisplay *self,
                                 uint32_t           logical_width,
                                 uint32_t           logical_height,
                                 uint32_t           refresh_hz,
                                 gboolean           hidpi,
                                 GError           **error)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), FALSE);
  g_return_val_if_fail (self->display != nil, FALSE);

  /* HiDPI flip in place is silently ignored by WindowServer; recreate. */
  if (hidpi != self->is_hidpi)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "reconfigure: hidpi flag change not supported in place "
                   "(%s → %s)", self->is_hidpi ? "hidpi" : "non-hidpi",
                   hidpi ? "hidpi" : "non-hidpi");
      return FALSE;
    }

  uint32_t need_w = hidpi ? logical_width * 2 : logical_width;
  uint32_t need_h = hidpi ? logical_height * 2 : logical_height;
  if (need_w > self->max_pixels_wide || need_h > self->max_pixels_high)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "reconfigure: new backing %ux%u exceeds descriptor max "
                   "%ux%u", need_w, need_h,
                   self->max_pixels_wide, self->max_pixels_high);
      return FALSE;
    }

  @autoreleasepool {
    CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
    settings.hiDPI = hidpi ? 1 : 0;
    settings.modes = @[
      [[CGVirtualDisplayMode alloc] initWithWidth:logical_width
                                           height:logical_height
                                      refreshRate:(CGFloat) refresh_hz]
    ];

    if (![self->display applySettings:settings])
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "CGVirtualDisplay applySettings failed during reconfigure");
        return FALSE;
      }
  }

  self->logical_width = logical_width;
  self->logical_height = logical_height;
  self->refresh_hz = refresh_hz;

  g_message ("Virtual display reconfigured in place: id=%u  %ux%u%s @ %uHz",
             self->display_id, logical_width, logical_height,
             hidpi ? " (HiDPI)" : "", refresh_hz);

  return TRUE;
}

gboolean
mrd_virtual_display_make_primary (MrdVirtualDisplay *self)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), FALSE);

  if (self->display_id == 0)
    return FALSE;

  /* Wait for WS registration. */
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

  /* Mirror in a separate transaction — VDs sometimes refuse this leg. */
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

gboolean
mrd_virtual_display_place_extended (MrdVirtualDisplay *self,
                                     MrdVdSide          side)
{
  g_return_val_if_fail (MRD_IS_VIRTUAL_DISPLAY (self), FALSE);

  if (self->display_id == 0)
    return FALSE;

  /* Wait for a non-VD online display to anchor against. */
  CGDirectDisplayID anchor = kCGNullDirectDisplay;
  CGDirectDisplayID online[16];
  uint32_t online_n = 0;
  for (int attempt = 0; attempt < 10 && anchor == kCGNullDirectDisplay; attempt++)
    {
      if (CGGetOnlineDisplayList (16, online, &online_n) != kCGErrorSuccess)
        online_n = 0;
      for (uint32_t i = 0; i < online_n; i++)
        {
          if (online[i] == self->display_id)
            continue;
          CGRect b = CGDisplayBounds (online[i]);
          if (!CGRectIsNull (b) && !CGRectIsEmpty (b))
            {
              anchor = online[i];
              break;
            }
        }
      if (anchor == kCGNullDirectDisplay)
        g_usleep (100000);
    }
  if (anchor == kCGNullDirectDisplay)
    {
      g_warning ("place_extended: no other display online — leaving VD as-is");
      return TRUE;
    }

  /* Anchor (NOT CGMainDisplayID — VD may be plist-promoted to primary). */
  CGRect anchor_bounds = CGDisplayBounds (anchor);
  CGRect vd_bounds = CGDisplayBounds (self->display_id);
  uint32_t vd_w = !CGRectIsNull (vd_bounds) && !CGRectIsEmpty (vd_bounds)
                    ? (uint32_t) vd_bounds.size.width
                    : self->logical_width;
  int32_t target_x = (side == MRD_VD_SIDE_LEFT)
    ? (int32_t) anchor_bounds.origin.x - (int32_t) vd_w
    : (int32_t) (anchor_bounds.origin.x + anchor_bounds.size.width);
  int32_t target_y = (int32_t) anchor_bounds.origin.y;

  /* Permanently writes plist so future sessions skip this transaction;
   * single shot — extra CGCompletes cause visible monitor flicker. */
  CGDisplayConfigRef config;
  if (CGBeginDisplayConfiguration (&config) != kCGErrorSuccess)
    {
      g_warning ("place_extended: CGBeginDisplayConfiguration failed");
      return FALSE;
    }

  CGConfigureDisplayMirrorOfDisplay (config, self->display_id,
                                     kCGNullDirectDisplay);
  for (uint32_t i = 0; i < online_n; i++)
    {
      if (online[i] != self->display_id &&
          CGDisplayMirrorsDisplay (online[i]) == self->display_id)
        CGConfigureDisplayMirrorOfDisplay (config, online[i],
                                           kCGNullDirectDisplay);
    }
  CGConfigureDisplayOrigin (config, anchor, 0, 0);
  CGConfigureDisplayOrigin (config, self->display_id, target_x, target_y);

  CGError err = CGCompleteDisplayConfiguration (config,
                                                kCGConfigurePermanently);
  if (err != kCGErrorSuccess)
    {
      g_warning ("place_extended: CGCompleteDisplayConfiguration failed (%d)", err);
      CGCancelDisplayConfiguration (config);
      return FALSE;
    }

  self->was_mirrored = FALSE;
  g_message ("VD %u placed extended on the %s at (%d, %d) (anchor=%u)",
             self->display_id,
             side == MRD_VD_SIDE_LEFT ? "left" : "right",
             target_x, target_y, anchor);
  return TRUE;
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
      g_message ("Virtual display destroyed: id=%u (releasing CGVirtualDisplay)",
                 self->display_id);
      @autoreleasepool {
        self->display = nil;
      }
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
