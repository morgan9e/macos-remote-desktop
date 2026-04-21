#include "mrd-session-rdp-private.h"

#include <Accelerate/Accelerate.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/pointer.h>

#include "../platform/mrd-cursor-capture.h"
#include "../platform/mrd-input-injector.h"

static void
send_cursor_bitmap (MrdSessionRdp   *session,
                    MrdCursorInfo   *cursor)
{
  freerdp_peer *peer = session->peer;
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  rdpUpdate *update = peer->context->update;

  if (!cursor->bitmap || cursor->width == 0 || cursor->height == 0)
    return;

  if (!peer_ctx->activated)
    return;

  uint16_t width = cursor->width;
  uint16_t height = cursor->height;
  uint16_t hotspot_x = cursor->hotspot_x;
  uint16_t hotspot_y = cursor->hotspot_y;
  uint8_t *bitmap = cursor->bitmap;
  uint32_t bitmap_size = cursor->bitmap_size;
  uint8_t *scaled_bitmap = NULL;

  gboolean needs_scale = (session->cursor_scale > 0.0f &&
                          (session->cursor_scale < 0.99f || session->cursor_scale > 1.01f));
  if (needs_scale)
    {
      uint16_t new_w = (uint16_t)(width * session->cursor_scale + 0.5f);
      uint16_t new_h = (uint16_t)(height * session->cursor_scale + 0.5f);
      if (new_w < 1) new_w = 1;
      if (new_h < 1) new_h = 1;
      if (new_w > 256) new_w = 256;
      if (new_h > 256) new_h = 256;

      size_t new_stride = new_w * 4;
      size_t new_size = new_h * new_stride;
      scaled_bitmap = g_malloc (new_size);

      vImage_Buffer src = {
        .data = bitmap,
        .height = height,
        .width = width,
        .rowBytes = (size_t) width * 4,
      };
      vImage_Buffer dst = {
        .data = scaled_bitmap,
        .height = new_h,
        .width = new_w,
        .rowBytes = new_stride,
      };
      vImage_Error verr = vImageScale_ARGB8888 (&src, &dst, NULL,
                                                kvImageHighQualityResampling);
      if (verr != kvImageNoError)
        {
          g_warning ("vImageScale_ARGB8888 failed (%ld); dropping cursor frame",
                     (long) verr);
          g_free (scaled_bitmap);
          return;
        }

      width = new_w;
      height = new_h;
      hotspot_x = (uint16_t)(cursor->hotspot_x * session->cursor_scale + 0.5f);
      hotspot_y = (uint16_t)(cursor->hotspot_y * session->cursor_scale + 0.5f);
      bitmap = scaled_bitmap;
      bitmap_size = (uint32_t)new_size;
    }

  if (width > 384 || height > 384)
    {
      g_warning ("Cursor too large: %ux%u (max 384x384)", width, height);
      g_free (scaled_bitmap);
      return;
    }

  /* Clamp hotspot — scaling can push it to or past the edge, and strict
   * clients (FreeRDP SDL frontend) reject out-of-bounds hotspots. */
  if (hotspot_x >= width)  hotspot_x = width  - 1;
  if (hotspot_y >= height) hotspot_y = height - 1;

  /* AND mask: 1bpp, 2-byte row pad. 1=transparent, 0=use XOR. */
  uint32_t and_stride = ((width + 15) / 16) * 2;
  uint32_t and_size = and_stride * height;
  uint8_t *and_mask = g_malloc0 (and_size);

  for (uint16_t row = 0; row < height; row++)
    {
      for (uint16_t col = 0; col < width; col++)
        {
          uint8_t alpha = bitmap[(row * width + col) * 4 + 3];
          if (alpha == 0)
            and_mask[row * and_stride + col / 8] |= (0x80 >> (col % 8));
        }
    }

  if (width <= 96 && height <= 96)
    {
      POINTER_NEW_UPDATE pointer_new = { 0 };
      pointer_new.xorBpp = 32;
      pointer_new.colorPtrAttr.cacheIndex = session->cursor_cache_index;
      pointer_new.colorPtrAttr.hotSpotX = hotspot_x;
      pointer_new.colorPtrAttr.hotSpotY = hotspot_y;
      pointer_new.colorPtrAttr.width = width;
      pointer_new.colorPtrAttr.height = height;
      pointer_new.colorPtrAttr.lengthAndMask = and_size;
      pointer_new.colorPtrAttr.lengthXorMask = bitmap_size;
      pointer_new.colorPtrAttr.xorMaskData = bitmap;
      pointer_new.colorPtrAttr.andMaskData = and_mask;

      update->pointer->PointerNew (peer->context, &pointer_new);
    }
  else
    {
      POINTER_LARGE_UPDATE pointer_large = { 0 };
      pointer_large.xorBpp = 32;
      pointer_large.cacheIndex = session->cursor_cache_index;
      pointer_large.hotSpotX = hotspot_x;
      pointer_large.hotSpotY = hotspot_y;
      pointer_large.width = width;
      pointer_large.height = height;
      pointer_large.lengthAndMask = and_size;
      pointer_large.lengthXorMask = bitmap_size;
      pointer_large.xorMaskData = bitmap;
      pointer_large.andMaskData = and_mask;

      update->pointer->PointerLarge (peer->context, &pointer_large);
    }

  g_free (and_mask);
  g_free (scaled_bitmap);

  /* Activate it. */
  POINTER_CACHED_UPDATE pointer_cached = { 0 };
  pointer_cached.cacheIndex = session->cursor_cache_index;
  update->pointer->PointerCached (peer->context, &pointer_cached);

  g_debug ("Cursor bitmap sent: %ux%u, hotspot (%u,%u)%s",
           width, height, hotspot_x, hotspot_y,
           session->cursor_scale < 0.99f ? " (scaled)" : "");

  /* Rotate so alternating shapes (arrow ↔ I-beam) cache separately. */
  session->cursor_cache_index = (uint16_t) ((session->cursor_cache_index + 1) %
                                            MRD_CURSOR_CACHE_SIZE);
}

void
mrd_session_update_cursor_if_changed (MrdSessionRdp *session)
{
  MrdCursorInfo current = { 0 };

  /* SLS seed is a cheap shape-change counter; -1 = NSCursor fallback. */
  int seed = mrd_cursor_capture_get_seed ();
  if (seed >= 0 && seed == session->last_cursor_seed)
    return;

  if (!mrd_cursor_capture_get_current (&current))
    return;

  session->last_cursor_seed = seed;

  if (mrd_cursor_capture_compare (&current, &session->last_cursor))
    {
      send_cursor_bitmap (session, &current);
      g_free (session->last_cursor.bitmap);
      session->last_cursor = current;
    }
  else
    {
      g_free (current.bitmap);
    }
}

/* Surfaces host-driven cursor moves; 1px deadband absorbs scaling noise. */
void
mrd_session_poll_server_cursor_position (MrdSessionRdp *session)
{
  if (!session->cursor_initialized || !session->input_injector)
    return;

  guint16 px = 0, py = 0;
  if (!mrd_input_injector_get_client_cursor_position (session->input_injector,
                                                      &px, &py))
    return;

  int dx = (int) px - (int) session->cursor_x;
  int dy = (int) py - (int) session->cursor_y;
  if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
    return;

  freerdp_peer *peer = session->peer;
  if (!peer || !peer->context || !peer->context->update)
    return;

  POINTER_POSITION_UPDATE pos = { 0 };
  pos.xPos = px;
  pos.yPos = py;
  peer->context->update->pointer->PointerPosition (peer->context, &pos);

  session->cursor_x = px;
  session->cursor_y = py;
}
