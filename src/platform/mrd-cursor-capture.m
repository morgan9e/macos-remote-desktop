/*
 * Primary: SLSGetGlobalCursorData (system-wide cursor, dlsym'd for
 * graceful degradation). Fallback: NSCursor (caller-process only,
 * always arrow for a background daemon).
 */

#include "mrd-cursor-capture.h"
#include "SLSCursorPrivate.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>

typedef struct {
  SLSMainConnectionIDFn        main_connection_id;
  SLSGetCurrentCursorSeedFn    get_current_cursor_seed;
  SLSGetGlobalCursorDataSizeFn get_global_cursor_data_size;
  SLSGetGlobalCursorDataFn     get_global_cursor_data;
  int                          cid;
  gboolean                     available;
} SlsCursorApi;

static SlsCursorApi *
sls_api (void)
{
  static SlsCursorApi api;
  static gsize init = 0;

  if (g_once_init_enter (&init))
    {
      api.main_connection_id =
        (SLSMainConnectionIDFn) dlsym (RTLD_DEFAULT, "SLSMainConnectionID");
      api.get_current_cursor_seed =
        (SLSGetCurrentCursorSeedFn) dlsym (RTLD_DEFAULT, "SLSGetCurrentCursorSeed");
      api.get_global_cursor_data_size =
        (SLSGetGlobalCursorDataSizeFn) dlsym (RTLD_DEFAULT, "SLSGetGlobalCursorDataSize");
      api.get_global_cursor_data =
        (SLSGetGlobalCursorDataFn) dlsym (RTLD_DEFAULT, "SLSGetGlobalCursorData");

      api.available = api.main_connection_id != NULL &&
                      api.get_current_cursor_seed != NULL &&
                      api.get_global_cursor_data_size != NULL &&
                      api.get_global_cursor_data != NULL;
      if (api.available)
        api.cid = api.main_connection_id ();
      else
        g_warning ("SLS cursor API unavailable (dlsym failed); "
                   "falling back to per-process NSCursor — only the "
                   "default arrow will be sent to clients");

      g_once_init_leave (&init, 1);
    }

  return &api;
}

int
mrd_cursor_capture_get_seed (void)
{
  SlsCursorApi *api = sls_api ();
  if (!api->available)
    return -1;
  return api->get_current_cursor_seed (api->cid);
}

/* FALSE → fall through to NSCursor. */
static gboolean
capture_via_sls (MrdCursorInfo *info)
{
  SlsCursorApi *api = sls_api ();
  if (!api->available)
    return FALSE;

  size_t size = 0;
  if (api->get_global_cursor_data_size (api->cid, &size) != kCGErrorSuccess ||
      size == 0 || size > 16 * 1024 * 1024)
    return FALSE;

  uint8_t *raw = g_malloc (size);
  int row_bytes = 0;
  CGRect bounds = CGRectZero;
  CGPoint hot_spot = CGPointZero;
  int bpp = 0, components = 0, bpc = 0;
  size_t out_size = size;

  CGError err = api->get_global_cursor_data (api->cid, raw, &out_size,
                                             &row_bytes, &bounds, &hot_spot,
                                             &bpp, &components, &bpc);
  if (err != kCGErrorSuccess || bpp != 32 || bpc != 8 ||
      bounds.size.width <= 0 || bounds.size.height <= 0)
    {
      g_free (raw);
      return FALSE;
    }

  size_t width = (size_t) bounds.size.width;
  size_t height = (size_t) bounds.size.height;

  /* PointerLarge max. */
  if (width > 384 || height > 384)
    {
      g_free (raw);
      return FALSE;
    }

  /* SLS rows are padded; wire format wants tight rows. */
  size_t tight_stride = width * 4;
  size_t tight_size = tight_stride * height;
  uint8_t *tight = g_malloc (tight_size);
  for (size_t y = 0; y < height; y++)
    memcpy (tight + y * tight_stride, raw + y * (size_t) row_bytes, tight_stride);
  g_free (raw);

  /* Un-premultiply: strict clients want straight alpha in the XOR mask. */
  for (size_t i = 0; i < tight_size; i += 4)
    {
      uint8_t a = tight[i + 3];
      if (a > 0 && a < 255)
        {
          tight[i + 0] = (tight[i + 0] * 255) / a;
          tight[i + 1] = (tight[i + 1] * 255) / a;
          tight[i + 2] = (tight[i + 2] * 255) / a;
        }
    }

  /* Strict clients (SDL FreeRDP) reject edge hotspots. */
  if (hot_spot.x < 0) hot_spot.x = 0;
  if (hot_spot.y < 0) hot_spot.y = 0;
  if (hot_spot.x >= (CGFloat) width)  hot_spot.x = (CGFloat) (width  - 1);
  if (hot_spot.y >= (CGFloat) height) hot_spot.y = (CGFloat) (height - 1);

  info->bitmap = tight;
  info->width = (uint16_t) width;
  info->height = (uint16_t) height;
  info->hotspot_x = (uint16_t) hot_spot.x;
  info->hotspot_y = (uint16_t) hot_spot.y;
  info->bitmap_size = (uint32_t) tight_size;
  return TRUE;
}

static gboolean
capture_via_nscursor (MrdCursorInfo *info)
{
  @autoreleasepool {
    NSCursor *cursor = [NSCursor currentSystemCursor];
    if (!cursor)
      return FALSE;

    NSImage *image = [cursor image];
    if (!image)
      return FALSE;

    NSPoint hotspot = [cursor hotSpot];
    NSSize size = [image size];

    CGImageRef cgImage = [image CGImageForProposedRect:NULL
                                               context:nil
                                                 hints:nil];
    if (!cgImage)
      return FALSE;

    size_t width = CGImageGetWidth (cgImage);
    size_t height = CGImageGetHeight (cgImage);
    CGFloat scale_x = (size.width  > 0) ? (CGFloat) width  / size.width  : 1.0;
    CGFloat scale_y = (size.height > 0) ? (CGFloat) height / size.height : 1.0;
    hotspot.x *= scale_x;
    hotspot.y *= scale_y;

    if (width > 384 || height > 384)
      return FALSE;

    size_t stride = width * 4;
    size_t bitmap_size = height * stride;
    uint8_t *bgra_data = g_malloc (bitmap_size);

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB ();
    CGContextRef ctx = CGBitmapContextCreate (bgra_data,
                                              width, height,
                                              8, stride,
                                              colorSpace,
                                              kCGImageAlphaPremultipliedFirst |
                                                kCGBitmapByteOrder32Little);
    CGColorSpaceRelease (colorSpace);

    if (!ctx)
      {
        g_free (bgra_data);
        return FALSE;
      }

    CGContextTranslateCTM (ctx, 0, height);
    CGContextScaleCTM (ctx, 1.0, -1.0);
    CGContextDrawImage (ctx, CGRectMake (0, 0, width, height), cgImage);
    CGContextRelease (ctx);

    for (size_t i = 0; i < bitmap_size; i += 4)
      {
        uint8_t a = bgra_data[i + 3];
        if (a > 0 && a < 255)
          {
            bgra_data[i + 0] = (bgra_data[i + 0] * 255) / a;
            bgra_data[i + 1] = (bgra_data[i + 1] * 255) / a;
            bgra_data[i + 2] = (bgra_data[i + 2] * 255) / a;
          }
      }

    if (hotspot.x < 0) hotspot.x = 0;
    if (hotspot.y < 0) hotspot.y = 0;
    if (hotspot.x >= (CGFloat) width)  hotspot.x = (CGFloat) (width  - 1);
    if (hotspot.y >= (CGFloat) height) hotspot.y = (CGFloat) (height - 1);

    info->bitmap = bgra_data;
    info->width = (uint16_t) width;
    info->height = (uint16_t) height;
    info->hotspot_x = (uint16_t) hotspot.x;
    info->hotspot_y = (uint16_t) hotspot.y;
    info->bitmap_size = (uint32_t) bitmap_size;
    return TRUE;
  }
}

gboolean
mrd_cursor_capture_get_current (MrdCursorInfo *info)
{
  g_return_val_if_fail (info != NULL, FALSE);
  memset (info, 0, sizeof (MrdCursorInfo));

  if (capture_via_sls (info))
    return TRUE;
  return capture_via_nscursor (info);
}

gboolean
mrd_cursor_capture_compare (const MrdCursorInfo *a,
                             const MrdCursorInfo *b)
{
  if (!a || !b)
    return TRUE;

  if (a->width != b->width ||
      a->height != b->height ||
      a->hotspot_x != b->hotspot_x ||
      a->hotspot_y != b->hotspot_y)
    return TRUE;

  if (!a->bitmap || !b->bitmap)
    return TRUE;

  return memcmp (a->bitmap, b->bitmap, a->bitmap_size) != 0;
}
