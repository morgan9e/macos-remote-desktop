#include "mrd-cursor-capture.h"
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
gboolean
mrd_cursor_capture_get_current (MrdCursorInfo *info)
{
  g_return_val_if_fail (info != NULL, FALSE);
  memset (info, 0, sizeof (MrdCursorInfo));
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
    CGFloat scale_x = (size.width  > 0) ? (CGFloat)width  / size.width  : 1.0;
    CGFloat scale_y = (size.height > 0) ? (CGFloat)height / size.height : 1.0;
    hotspot.x *= scale_x;
    hotspot.y *= scale_y;
    
    if (width > 384 || height > 384)
      {
        
        return FALSE;
      }
    
    size_t stride = width * 4;
    size_t bitmap_size = height * stride;
    
    uint8_t *bgra_data = g_malloc (bitmap_size);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB ();
    
    CGContextRef ctx = CGBitmapContextCreate (bgra_data,
                                               width, height,
                                               8, stride,
                                               colorSpace,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
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
        uint8_t b = bgra_data[i + 0];
        uint8_t g = bgra_data[i + 1];
        uint8_t r = bgra_data[i + 2];
        uint8_t a = bgra_data[i + 3];
        if (a > 0 && a < 255)
          {
            bgra_data[i + 0] = (b * 255) / a;
            bgra_data[i + 1] = (g * 255) / a;
            bgra_data[i + 2] = (r * 255) / a;
          }
      }
    info->bitmap = bgra_data;
    info->width = (uint16_t)width;
    info->height = (uint16_t)height;
    info->hotspot_x = (uint16_t)hotspot.x;
    info->hotspot_y = (uint16_t)hotspot.y;
    info->bitmap_size = (uint32_t)bitmap_size;
    return TRUE;
  }
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