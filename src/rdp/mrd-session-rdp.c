#include "mrd-session-rdp.h"
#include <gio/gio.h>
#include <cairo.h>
#include <CoreVideo/CoreVideo.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/pointer.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include "../mrd-rdp-private.h"
#include "mrd-rdp-server.h"
#include "mrd-rdp-graphics-pipeline.h"
#include "../platform/mrd-screen-capture.h"
#include "../platform/mrd-input-injector.h"
#include "../platform/mrd-cursor-capture.h"
#include "../platform/mrd-virtual-display.h"
#include "../encoding/mrd-encode-session.h"
#include "../util/mrd-bitstream.h"
#include "../util/mrd-damage-utils.h"
#include <IOKit/pwr_mgt/IOPMLib.h>
struct _MrdSessionRdp
{
  GObject parent;
  MrdRdpServer *server;
  freerdp_peer *peer;
  char *cert_file;
  char *key_file;
  GThread *session_thread;
  gboolean session_should_stop;
  HANDLE stop_event;
  
  MrdVirtualDisplay *virtual_display;
  MrdScreenCapture *screen_capture;
  MrdInputInjector *input_injector;
  
  MrdEncodeSession *encode_session;
  
  MrdRdpGraphicsPipeline *graphics_pipeline;
  HANDLE vcm;
  gboolean drdynvc_ready;
  
  uint16_t surface_id;
  uint32_t surface_width;
  uint32_t surface_height;
  gboolean surface_created;
  gint64 next_frame_us;
  gint64 frame_period_us;
  
  uint8_t *prev_frame_buffer;
  uint32_t prev_frame_size;
  uint32_t prev_frame_width;
  uint32_t prev_frame_height;
  
  uint16_t cursor_x;
  uint16_t cursor_y;
  gboolean cursor_initialized;
  MrdCursorInfo last_cursor;
  uint16_t cursor_cache_index;
  gint64 next_cursor_check_us;
};

#define MRD_CURSOR_POLL_US  (500 * 1000)   

#define MRD_FRAME_RATE_RFX  15
#define MRD_MIN_WAIT_MS     2   
G_DEFINE_TYPE (MrdSessionRdp, mrd_session_rdp, G_TYPE_OBJECT)

static BOOL
on_peer_capabilities (freerdp_peer *peer)
{
  rdpSettings *settings = peer->context->settings;
  gboolean rfx = freerdp_settings_get_bool (settings, FreeRDP_RemoteFxCodec);
  gboolean nsc = freerdp_settings_get_bool (settings, FreeRDP_NSCodec);
  gboolean gfx = freerdp_settings_get_bool (settings, FreeRDP_SupportGraphicsPipeline);
  
  g_message ("Client capabilities: RFX=%s, NSC=%s, GFX=%s",
             rfx ? "yes" : "no",
             nsc ? "yes" : "no",
             gfx ? "yes" : "no");
  return TRUE;
}

#define MIN_VD_WIDTH   640
#define MIN_VD_HEIGHT  480
#define MAX_VD_WIDTH   3840
#define MAX_VD_HEIGHT  2160
static BOOL
on_peer_post_connect (freerdp_peer *peer)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  rdpSettings *settings = peer->context->settings;
  g_autoptr(GError) error = NULL;
  uint32_t client_width = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth);
  uint32_t client_height = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight);
  g_message ("Peer connected: %s (requested %ux%u)",
             peer->hostname, client_width, client_height);
  
  client_width = CLAMP (client_width, MIN_VD_WIDTH, MAX_VD_WIDTH);
  client_height = CLAMP (client_height, MIN_VD_HEIGHT, MAX_VD_HEIGHT);
  
  uint32_t scale = 100;
  uint32_t client_scale = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopScaleFactor);
  if (client_scale > 100 && client_scale <= 500)
    {
      scale = client_scale;
      g_message ("Client scale hint: %u%%", scale);
    }
  
  extern int g_hidpi;
  gboolean use_hidpi;
  if (g_hidpi == 0)
    use_hidpi = FALSE;
  else if (g_hidpi == 1)
    use_hidpi = TRUE;
  else 
    use_hidpi = (scale > 100);
  
  if (scale > 100)
    {
      
      uint32_t logical_w = (client_width * 100) / scale;
      uint32_t logical_h = (client_height * 100) / scale;
      logical_w = (logical_w + 1) & ~1;
      logical_h = (logical_h + 1) & ~1;
      g_message ("Scaled VD: %ux%u at %u%% → %ux%u logical%s",
                 client_width, client_height, scale, logical_w, logical_h,
                 use_hidpi ? " (HiDPI)" : "");
      if (use_hidpi)
        session->virtual_display = mrd_virtual_display_new_hidpi (
          logical_w, logical_h, 60, &error);
      else
        session->virtual_display = mrd_virtual_display_new (
          logical_w, logical_h, 60, &error);
    }
  else if (use_hidpi)
    {
      
      session->virtual_display = mrd_virtual_display_new_hidpi (
        client_width, client_height, 60, &error);
    }
  else
    {
      session->virtual_display = mrd_virtual_display_new (client_width, client_height, 60, &error);
    }
  if (!session->virtual_display)
    {
      g_warning ("Failed to create virtual display: %s", error->message);
      return FALSE;
    }
  uint32_t vd_id = mrd_virtual_display_get_id (session->virtual_display);
  
  mrd_virtual_display_make_primary (session->virtual_display);
  mrd_virtual_display_mirror_physical (session->virtual_display);
  
  uint32_t vd_w = 0, vd_h = 0;
  mrd_virtual_display_get_logical_size (session->virtual_display, &vd_w, &vd_h);
  gboolean needs_scaling = (vd_w != client_width || vd_h != client_height) ||
                            mrd_virtual_display_is_hidpi (session->virtual_display);
  if (needs_scaling)
    {
      session->screen_capture = mrd_screen_capture_new_scaled (vd_id, client_width, client_height);
      g_message ("Screen capture: scaling to %ux%u", client_width, client_height);
    }
  else
    {
      session->screen_capture = mrd_screen_capture_new_for_display (vd_id);
    }
  if (!mrd_screen_capture_start (session->screen_capture, NULL))
    {
      g_warning ("Failed to start screen capture");
      return FALSE;
    }
  
  session->input_injector = mrd_input_injector_new ();
  mrd_input_injector_set_target_display (session->input_injector, vd_id);
  mrd_input_injector_set_client_size (session->input_injector, client_width, client_height);
  
  session->encode_session = mrd_encode_session_new (FALSE, FALSE);
  
  session->vcm = peer_ctx->vcm;
  return TRUE;
}
static void
send_frame (freerdp_peer    *peer,
            const uint8_t   *pixels,
            uint32_t         width,
            uint32_t         height,
            uint32_t         stride,
            cairo_region_t  *damage_region)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  rdpUpdate *update = peer->context->update;
  rdpSettings *settings = peer->context->settings;
  wStream *s = peer_ctx->encode_stream;
  if (peer_ctx->codec == MRD_CODEC_NONE)
    return;
  Stream_SetPosition (s, 0);
  SURFACE_FRAME_MARKER fm_begin = {
    .frameAction = SURFACECMD_FRAMEACTION_BEGIN,
    .frameId = peer_ctx->frame_id,
  };
  update->SurfaceFrameMarker (update->context, &fm_begin);
  SURFACE_BITS_COMMAND cmd = { 0 };
  cmd.destLeft = 0;
  cmd.destTop = 0;
  cmd.destRight = width;
  cmd.destBottom = height;
  cmd.bmp.bpp = 32;
  cmd.bmp.width = (UINT16)width;
  cmd.bmp.height = (UINT16)height;
  if (peer_ctx->codec == MRD_CODEC_RFX)
    {
      int n_rects = damage_region ? cairo_region_num_rectangles (damage_region) : 1;
      RFX_RECT *rects = g_new0 (RFX_RECT, n_rects);
      if (damage_region && n_rects > 0)
        {
          for (int i = 0; i < n_rects; i++)
            {
              cairo_rectangle_int_t r;
              cairo_region_get_rectangle (damage_region, i, &r);
              
              if (r.x < 0) r.x = 0;
              if (r.y < 0) r.y = 0;
              if (r.x + r.width > (int)width) r.width = (int)width - r.x;
              if (r.y + r.height > (int)height) r.height = (int)height - r.y;
              
              if (r.width <= 0 || r.height <= 0)
                continue;
              rects[i].x = (UINT16)r.x;
              rects[i].y = (UINT16)r.y;
              rects[i].width = (UINT16)r.width;
              rects[i].height = (UINT16)r.height;
            }
        }
      else
        {
          
          rects[0].x = 0;
          rects[0].y = 0;
          rects[0].width = (UINT16)width;
          rects[0].height = (UINT16)height;
        }
      if (!rfx_compose_message (peer_ctx->rfx_context, s, rects, n_rects,
                                pixels, width, height, stride))
        {
          g_warning ("rfx_compose_message failed (frame %u)", peer_ctx->frame_id);
          g_free (rects);
          return;
        }
      g_free (rects);
      cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
      cmd.bmp.codecID = freerdp_settings_get_uint32 (settings, FreeRDP_RemoteFxCodecId);
      g_debug ("RFX encoded: frame %u, %d rects, %zu bytes",
               peer_ctx->frame_id, n_rects, Stream_GetPosition (s));
    }
  else 
    {
      
      if (!nsc_compose_message (peer_ctx->nsc_context, s,
                                pixels, width, height, stride))
        {
          g_warning ("nsc_compose_message failed (frame %u)", peer_ctx->frame_id);
          return;
        }
      cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
      cmd.bmp.codecID = freerdp_settings_get_uint32 (settings, FreeRDP_NSCodecId);
      g_debug ("NSC encoded: frame %u, %zu bytes", peer_ctx->frame_id, Stream_GetPosition (s));
    }
  cmd.bmp.bitmapDataLength = Stream_GetPosition (s);
  cmd.bmp.bitmapData = Stream_Buffer (s);
  update->SurfaceBits (update->context, &cmd);
  SURFACE_FRAME_MARKER fm_end = {
    .frameAction = SURFACECMD_FRAMEACTION_END,
    .frameId = peer_ctx->frame_id,
  };
  update->SurfaceFrameMarker (update->context, &fm_end);
  peer_ctx->frame_id++;
}
static void
pump_frame (MrdSessionRdp *session)
{
  freerdp_peer *peer = session->peer;
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  static guint64 frame_count = 0;
  static guint64 fps_frame_count = 0;
  static guint64 skipped_count = 0;
  static gint64 fps_last_time = 0;
  if (!peer_ctx->activated)
    return;
  cairo_region_t *damage_region = NULL;
  uint32_t width = 0, height = 0;
  if (peer_ctx->codec == MRD_CODEC_GFX && session->surface_created)
    {
      
      CVPixelBufferRef pb = (CVPixelBufferRef)
        mrd_screen_capture_take_pixel_buffer (session->screen_capture);
      if (!pb)
        {
          g_debug ("pump_frame: no NV12 frame available");
          return;
        }
      width  = (uint32_t)CVPixelBufferGetWidth (pb);
      height = (uint32_t)CVPixelBufferGetHeight (pb);
      
      CVPixelBufferLockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
      uint8_t *y_plane   = CVPixelBufferGetBaseAddressOfPlane (pb, 0);
      uint32_t y_stride  = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane (pb, 0);
      uint32_t y_size    = y_stride * height;
      if (session->prev_frame_buffer &&
          (session->prev_frame_width != width ||
           session->prev_frame_height != height ||
           session->prev_frame_size != y_size))
        {
          g_clear_pointer (&session->prev_frame_buffer, g_free);
          session->prev_frame_size = 0;
        }
      damage_region = mrd_get_damage_region (y_plane,
                                             session->prev_frame_buffer,
                                             width, height, y_stride, 1);
      int n_rects = cairo_region_num_rectangles (damage_region);
      if (n_rects == 0)
        {
          cairo_region_destroy (damage_region);
          CVPixelBufferUnlockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
          CVPixelBufferRelease (pb);
          skipped_count++;
          return;
        }
      if (!session->prev_frame_buffer)
        {
          session->prev_frame_buffer = g_malloc (y_size);
          session->prev_frame_size = y_size;
          session->prev_frame_width = width;
          session->prev_frame_height = height;
        }
      memcpy (session->prev_frame_buffer, y_plane, y_size);
      CVPixelBufferUnlockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
      MrdBitstream *main_bs = NULL, *aux_bs = NULL;
      g_autoptr(GError) error = NULL;
      if (mrd_encode_session_encode_pixel_buffer (session->encode_session,
                                                  pb, &main_bs, &aux_bs,
                                                  &error))
        {
          if (!mrd_rdp_graphics_pipeline_submit_frame (session->graphics_pipeline,
                                                        session->surface_id,
                                                        main_bs, aux_bs,
                                                        damage_region, &error))
            g_warning ("GFX submit failed: %s", error->message);
          mrd_bitstream_free (main_bs);
          if (aux_bs)
            mrd_bitstream_free (aux_bs);
        }
      else
        {
          g_warning ("H.264 encode failed: %s", error->message);
        }
      cairo_region_destroy (damage_region);
      CVPixelBufferRelease (pb);
    }
  else
    {
      
      uint32_t stride = 0;
      uint8_t *pixels = mrd_screen_capture_get_frame (session->screen_capture,
                                                      &width, &height, &stride);
      if (!pixels)
        {
          g_debug ("pump_frame: no pixels from screen capture");
          return;
        }
      uint32_t frame_size = stride * height;
      if (session->prev_frame_buffer &&
          (session->prev_frame_width != width ||
           session->prev_frame_height != height ||
           session->prev_frame_size != frame_size))
        {
          g_clear_pointer (&session->prev_frame_buffer, g_free);
          session->prev_frame_size = 0;
        }
      damage_region = mrd_get_damage_region (pixels,
                                             session->prev_frame_buffer,
                                             width, height, stride, 4);
      int n_rects = cairo_region_num_rectangles (damage_region);
      if (n_rects == 0)
        {
          cairo_region_destroy (damage_region);
          g_free (pixels);
          skipped_count++;
          return;
        }
      if (!session->prev_frame_buffer)
        {
          session->prev_frame_buffer = g_malloc (frame_size);
          session->prev_frame_size = frame_size;
          session->prev_frame_width = width;
          session->prev_frame_height = height;
        }
      memcpy (session->prev_frame_buffer, pixels, frame_size);
      send_frame (peer, pixels, width, height, stride, damage_region);
      cairo_region_destroy (damage_region);
      g_free (pixels);
    }
  frame_count++;
  fps_frame_count++;
  
  gint64 now = g_get_monotonic_time ();
  if (fps_last_time == 0)
    fps_last_time = now;
  gint64 elapsed = now - fps_last_time;
  if (elapsed >= G_USEC_PER_SEC)
    {
      double fps = (double)fps_frame_count * G_USEC_PER_SEC / elapsed;
      g_message ("FPS: %.1f (frame #%"G_GUINT64_FORMAT", skipped %"G_GUINT64_FORMAT")",
                 fps, frame_count, skipped_count);
      fps_frame_count = 0;
      skipped_count = 0;
      fps_last_time = now;
    }
}
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
  
  if (cursor->width > 384 || cursor->height > 384)
    {
      g_warning ("Cursor too large: %ux%u (max 384x384)", cursor->width, cursor->height);
      return;
    }
  
  uint32_t and_stride = ((cursor->width + 15) / 16) * 2;
  uint32_t and_size = and_stride * cursor->height;
  uint8_t *and_mask = g_malloc0 (and_size);
  for (uint16_t row = 0; row < cursor->height; row++)
    {
      for (uint16_t col = 0; col < cursor->width; col++)
        {
          uint8_t alpha = cursor->bitmap[(row * cursor->width + col) * 4 + 3];
          if (alpha == 0)
            and_mask[row * and_stride + col / 8] |= (0x80 >> (col % 8));
        }
    }
  
  if (cursor->width <= 96 && cursor->height <= 96)
    {
      POINTER_NEW_UPDATE pointer_new = { 0 };
      pointer_new.xorBpp = 32;
      pointer_new.colorPtrAttr.cacheIndex = session->cursor_cache_index;
      pointer_new.colorPtrAttr.hotSpotX = cursor->hotspot_x;
      pointer_new.colorPtrAttr.hotSpotY = cursor->hotspot_y;
      pointer_new.colorPtrAttr.width = cursor->width;
      pointer_new.colorPtrAttr.height = cursor->height;
      pointer_new.colorPtrAttr.lengthAndMask = and_size;
      pointer_new.colorPtrAttr.lengthXorMask = cursor->bitmap_size;
      pointer_new.colorPtrAttr.xorMaskData = cursor->bitmap;
      pointer_new.colorPtrAttr.andMaskData = and_mask;
      update->pointer->PointerNew (peer->context, &pointer_new);
    }
  else
    {
      POINTER_LARGE_UPDATE pointer_large = { 0 };
      pointer_large.xorBpp = 32;
      pointer_large.cacheIndex = session->cursor_cache_index;
      pointer_large.hotSpotX = cursor->hotspot_x;
      pointer_large.hotSpotY = cursor->hotspot_y;
      pointer_large.width = cursor->width;
      pointer_large.height = cursor->height;
      pointer_large.lengthAndMask = and_size;
      pointer_large.lengthXorMask = cursor->bitmap_size;
      pointer_large.xorMaskData = cursor->bitmap;
      pointer_large.andMaskData = and_mask;
      update->pointer->PointerLarge (peer->context, &pointer_large);
    }
  g_free (and_mask);
  
  POINTER_CACHED_UPDATE pointer_cached = { 0 };
  pointer_cached.cacheIndex = session->cursor_cache_index;
  update->pointer->PointerCached (peer->context, &pointer_cached);
  g_debug ("Cursor bitmap sent: %ux%u, hotspot (%u,%u)",
           cursor->width, cursor->height, cursor->hotspot_x, cursor->hotspot_y);
}
static void
update_cursor_if_changed (MrdSessionRdp *session)
{
  MrdCursorInfo current = { 0 };

  if (!mrd_cursor_capture_get_current (&current))
    return;
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
static BOOL
on_peer_activate (freerdp_peer *peer)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  rdpSettings *settings = peer->context->settings;
  g_message ("Peer activated");
  
  if (session->input_injector)
    mrd_input_injector_release_modifiers (session->input_injector);
  
  gboolean client_rfx = freerdp_settings_get_bool (settings, FreeRDP_RemoteFxCodec);
  gboolean client_nsc = freerdp_settings_get_bool (settings, FreeRDP_NSCodec);
  uint32_t surface_cmds = freerdp_settings_get_uint32 (settings, FreeRDP_SurfaceCommandsSupported);
  g_message ("Client codecs: RFX=%s, NSC=%s, SurfaceCmds=0x%x",
             client_rfx ? "yes" : "no",
             client_nsc ? "yes" : "no",
             surface_cmds);
  
  gboolean stream_bits = (surface_cmds & SURFCMDS_STREAM_SURFACE_BITS) != 0;
  if (client_rfx && stream_bits)
    {
      peer_ctx->codec = MRD_CODEC_RFX;
      g_message ("Using RemoteFX codec");
    }
  else if (client_nsc)
    {
      peer_ctx->codec = MRD_CODEC_NSC;
      g_message ("Using NSCodec (RFX not supported by client)");
    }
  else
    {
      
      gboolean gfx_expected = freerdp_settings_get_bool (settings, FreeRDP_SupportGraphicsPipeline);
      if (gfx_expected)
        {
          peer_ctx->codec = MRD_CODEC_NONE;
          g_message ("No legacy codec; waiting for RDPGFX pipeline");
        }
      else
        {
          g_warning ("Client doesn't support RemoteFX, NSCodec, or GFX - cannot stream!");
          peer_ctx->codec = MRD_CODEC_NONE;
          return FALSE;
        }
    }
  
  int cap_width = 0, cap_height = 0;
  mrd_screen_capture_get_dimensions (session->screen_capture,
                                     &cap_width, &cap_height, NULL);
  uint32_t width = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopWidth);
  uint32_t height = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopHeight);
  if (cap_width > 0 && cap_height > 0 &&
      ((uint32_t)cap_width != width || (uint32_t)cap_height != height))
    {
      width = (uint32_t)cap_width;
      height = (uint32_t)cap_height;
      freerdp_settings_set_uint32 (settings, FreeRDP_DesktopWidth, width);
      freerdp_settings_set_uint32 (settings, FreeRDP_DesktopHeight, height);
      peer->context->update->DesktopResize (peer->context);
      g_message ("DesktopResize requested: %ux%u", width, height);
    }
  
  if (peer_ctx->codec == MRD_CODEC_RFX)
    {
      if (!rfx_context_reset (peer_ctx->rfx_context, width, height))
        {
          g_warning ("rfx_context_reset failed");
          return FALSE;
        }
      if (!rfx_context_set_mode (peer_ctx->rfx_context, RLGR3))
        {
          g_warning ("rfx_context_set_mode failed");
          return FALSE;
        }
      rfx_context_set_pixel_format (peer_ctx->rfx_context, PIXEL_FORMAT_BGRA32);
    }
  else if (peer_ctx->codec == MRD_CODEC_NSC)
    {
      if (!nsc_context_set_parameters (peer_ctx->nsc_context, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRA32))
        {
          g_warning ("nsc_context_set_parameters failed");
          return FALSE;
        }
    }
  
  peer_ctx->activated = TRUE;
  session->next_frame_us = g_get_monotonic_time ();
  if (peer_ctx->codec != MRD_CODEC_NONE)
    {
      session->frame_period_us = G_USEC_PER_SEC / MRD_FRAME_RATE_RFX;
      g_message ("%s frame streaming started at %d FPS (%ux%u)",
                 peer_ctx->codec == MRD_CODEC_RFX ? "RFX" : "NSC",
                 MRD_FRAME_RATE_RFX, width, height);
    }
  else
    {
      
      session->frame_period_us = 0;
    }
  
  session->cursor_initialized = TRUE;
  session->cursor_cache_index = 0;
  update_cursor_if_changed (session);
  
  pump_frame (session);
  return TRUE;
}
static BOOL
on_keyboard_event (rdpInput         *input,
                   UINT16            flags,
                   UINT8             code)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  if (session->input_injector)
    mrd_input_injector_handle_keyboard (session->input_injector, flags, code);
  return TRUE;
}
static BOOL
on_mouse_event (rdpInput        *input,
                UINT16           flags,
                UINT16           x,
                UINT16           y)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  if (session->input_injector)
    mrd_input_injector_handle_mouse (session->input_injector, flags, x, y);
  
  session->cursor_x = x;
  session->cursor_y = y;
  
  if (session->cursor_initialized)
    {
      rdpUpdate *update = input->context->update;
      POINTER_POSITION_UPDATE pos = { 0 };
      pos.xPos = x;
      pos.yPos = y;
      update->pointer->PointerPosition (input->context, &pos);
    }
  
  return TRUE;
}
static BOOL
on_extended_mouse_event (rdpInput        *input,
                         UINT16           flags,
                         UINT16           x,
                         UINT16           y)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  if (session->input_injector)
    mrd_input_injector_handle_extended_mouse (session->input_injector, flags, x, y);
  
  session->cursor_x = x;
  session->cursor_y = y;
  if (session->cursor_initialized)
    {
      rdpUpdate *update = input->context->update;
      POINTER_POSITION_UPDATE pos = { 0 };
      pos.xPos = x;
      pos.yPos = y;
      update->pointer->PointerPosition (input->context, &pos);
    }
  return TRUE;
}

static BOOL
peer_context_new (freerdp_peer   *peer,
                  RdpPeerContext *peer_ctx)
{
  peer_ctx->rfx_context = rfx_context_new (TRUE);
  if (!peer_ctx->rfx_context)
    return FALSE;
  peer_ctx->nsc_context = nsc_context_new ();
  if (!peer_ctx->nsc_context)
    {
      rfx_context_free (peer_ctx->rfx_context);
      return FALSE;
    }
  
  peer_ctx->encode_stream = Stream_New (NULL, 16 * 1024 * 1024);
  if (!peer_ctx->encode_stream)
    {
      nsc_context_free (peer_ctx->nsc_context);
      rfx_context_free (peer_ctx->rfx_context);
      return FALSE;
    }
  g_mutex_init (&peer_ctx->channel_mutex);
  
  peer_ctx->vcm = WTSOpenServerA ((LPSTR) peer->context);
  if (!peer_ctx->vcm || peer_ctx->vcm == INVALID_HANDLE_VALUE)
    {
      g_warning ("Failed to open virtual channel manager");
      Stream_Free (peer_ctx->encode_stream, TRUE);
      rfx_context_free (peer_ctx->rfx_context);
      g_mutex_clear (&peer_ctx->channel_mutex);
      return FALSE;
    }
  return TRUE;
}
static void
peer_context_free (freerdp_peer   *peer,
                   RdpPeerContext *peer_ctx)
{
  if (peer_ctx->vcm && peer_ctx->vcm != INVALID_HANDLE_VALUE)
    {
      WTSCloseServer (peer_ctx->vcm);
      peer_ctx->vcm = NULL;
    }
  g_mutex_clear (&peer_ctx->channel_mutex);
  if (peer_ctx->encode_stream)
    Stream_Free (peer_ctx->encode_stream, TRUE);
  if (peer_ctx->rfx_context)
    rfx_context_free (peer_ctx->rfx_context);
  if (peer_ctx->nsc_context)
    nsc_context_free (peer_ctx->nsc_context);
}

static gpointer
session_thread_func (gpointer data)
{
  MrdSessionRdp *session = MRD_SESSION_RDP (data);
  freerdp_peer *peer = session->peer;
  HANDLE events[35];  
  DWORD event_count;
  g_debug ("Session thread started");
  
  IOPMAssertionID no_sleep_assertion = kIOPMNullAssertionID;
  IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleDisplaySleep,
                               kIOPMAssertionLevelOn,
                               CFSTR ("RDP client connected"),
                               &no_sleep_assertion);
  while (!session->session_should_stop)
    {
      event_count = 0;
      
      events[event_count++] = session->stop_event;
      
      DWORD peer_count = peer->GetEventHandles (peer, &events[event_count], 32);
      if (peer_count == 0)
        {
          g_usleep (10000);  
          continue;
        }
      event_count += peer_count;
      
      if (session->vcm)
        {
          HANDLE vcm_event = WTSVirtualChannelManagerGetEventHandle (session->vcm);
          if (vcm_event)
            events[event_count++] = vcm_event;
        }
      
      HANDLE gfx_event_handle = NULL;
      if (session->graphics_pipeline)
        {
          gfx_event_handle = mrd_rdp_graphics_pipeline_get_event_handle (session->graphics_pipeline);
          if (gfx_event_handle)
            events[event_count++] = gfx_event_handle;
        }
      
      DWORD wait_ms = 100;
      RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
      if (peer_ctx->activated)
        {
          gint64 now = g_get_monotonic_time ();
          gint64 delta_us = session->next_frame_us - now;
          if (delta_us <= 0)
            wait_ms = MRD_MIN_WAIT_MS;  
          else
            wait_ms = MAX (MRD_MIN_WAIT_MS, MIN (100, (DWORD)(delta_us / 1000)));
        }
      DWORD status = WaitForMultipleObjects (event_count, events, FALSE, wait_ms);
      if (status == WAIT_FAILED)
        {
          if (session->session_should_stop)
            break;
          g_warning ("WaitForMultipleObjects failed");
          break;
        }
      if (session->session_should_stop)
        break;
      if (WaitForSingleObject (session->stop_event, 0) == WAIT_OBJECT_0)
        break;
      
      if (!peer->CheckFileDescriptor (peer))
        {
          g_debug ("Peer disconnected");
          break;
        }
      
      if (session->vcm)
        {
          if (!WTSVirtualChannelManagerCheckFileDescriptor (session->vcm))
            {
              g_warning ("VCM check failed");
              break;
            }
          
          if (!session->drdynvc_ready &&
              WTSVirtualChannelManagerIsChannelJoined (session->vcm, DRDYNVC_SVC_CHANNEL_NAME))
            {
              DWORD drdynvc_state = WTSVirtualChannelManagerGetDrdynvcState (session->vcm);
              if (drdynvc_state == DRDYNVC_STATE_READY)
                {
                  session->drdynvc_ready = TRUE;
                  g_message ("DRDYNVC ready, opening RDPGFX channel");
                  session->graphics_pipeline = mrd_rdp_graphics_pipeline_new (
                    session, session->vcm, peer->context);
                  if (session->graphics_pipeline)
                    {
                      if (!mrd_rdp_graphics_pipeline_open_channel (session->graphics_pipeline))
                        {
                          g_warning ("Failed to open RDPGFX channel, falling back to RFX");
                          g_clear_object (&session->graphics_pipeline);
                        }
                    }
                }
            }
        }
      
      if (session->graphics_pipeline && gfx_event_handle)
        {
          if (WaitForSingleObject (gfx_event_handle, 0) == WAIT_OBJECT_0)
            {
              if (!mrd_rdp_graphics_pipeline_handle_messages (session->graphics_pipeline))
                {
                  g_warning ("RDPGFX message handling failed, falling back to RFX");
                  if (session->surface_created)
                    session->surface_created = FALSE;
                  mrd_rdp_graphics_pipeline_stop (session->graphics_pipeline);
                  g_clear_object (&session->graphics_pipeline);
                  if (session->encode_session)
                    mrd_encode_session_stop (session->encode_session);
                  peer_ctx->codec = MRD_CODEC_RFX;
                  session->frame_period_us = G_USEC_PER_SEC / MRD_FRAME_RATE_RFX;
                }
            }
        }
      
      if (session->graphics_pipeline &&
          mrd_rdp_graphics_pipeline_needs_reset (session->graphics_pipeline))
        {
          int cap_width = 0, cap_height = 0;
          mrd_screen_capture_get_dimensions (session->screen_capture,
                                             &cap_width, &cap_height, NULL);
          mrd_rdp_graphics_pipeline_send_reset_graphics (session->graphics_pipeline,
                                                          (uint32_t)cap_width,
                                                          (uint32_t)cap_height);
          continue;  
        }
      
      if (session->graphics_pipeline && !session->surface_created &&
          mrd_rdp_graphics_pipeline_is_ready (session->graphics_pipeline))
        {
          g_message ("RDPGFX ready, setting up H.264 streaming");
          gboolean have_avc444 = FALSE, have_avc420 = FALSE;
          mrd_rdp_graphics_pipeline_get_capabilities (session->graphics_pipeline,
                                                       &have_avc444, &have_avc420);
          int cap_width = 0, cap_height = 0;
          mrd_screen_capture_get_dimensions (session->screen_capture,
                                             &cap_width, &cap_height, NULL);
          
          g_clear_object (&session->encode_session);
          session->encode_session = mrd_encode_session_new (have_avc444, have_avc420);
          g_autoptr(GError) gfx_error = NULL;
          if (!mrd_encode_session_start (session->encode_session,
                                         (uint32_t)cap_width, (uint32_t)cap_height,
                                         &gfx_error))
            {
              g_warning ("Failed to start H.264 encoder: %s", gfx_error->message);
            }
          else if (!mrd_rdp_graphics_pipeline_create_surface (session->graphics_pipeline,
                                                               0, (uint32_t)cap_width,
                                                               (uint32_t)cap_height, &gfx_error))
            {
              g_warning ("Failed to create GFX surface: %s", gfx_error->message);
            }
          else
            {
              session->surface_id = 0;
              session->surface_width = (uint32_t)cap_width;
              session->surface_height = (uint32_t)cap_height;
              session->surface_created = TRUE;
              peer_ctx->codec = MRD_CODEC_GFX;
              
              session->frame_period_us = 0;
              
              g_autoptr(GError) nv12_err = NULL;
              if (!mrd_screen_capture_enable_nv12 (session->screen_capture,
                                                   &nv12_err))
                g_warning ("NV12 capture switch failed: %s",
                           nv12_err->message);
              
              g_clear_pointer (&session->prev_frame_buffer, g_free);
              session->prev_frame_size = 0;
              g_message ("H.264 frame streaming started, uncapped (%dx%d)",
                         cap_width, cap_height);
            }
        }
      
      if (peer_ctx->activated && session->surface_created &&
          peer_ctx->codec == MRD_CODEC_GFX)
        {
          pump_frame (session);
        }
      else if (peer_ctx->activated && session->frame_period_us > 0)
        {
          gint64 now = g_get_monotonic_time ();
          if (now >= session->next_frame_us)
            {
              
              gint64 lag = now - session->next_frame_us;
              if (lag > 2 * session->frame_period_us)
                {
                  g_debug ("Frame skip: %"G_GINT64_FORMAT" ms behind",
                           lag / 1000);
                  session->next_frame_us = now;
                }
              pump_frame (session);
              session->next_frame_us += session->frame_period_us;
            }
        }
      
      if (peer_ctx->activated && session->cursor_initialized)
        {
          gint64 now_us = g_get_monotonic_time ();
          if (now_us >= session->next_cursor_check_us)
            {
              update_cursor_if_changed (session);
              session->next_cursor_check_us = now_us + MRD_CURSOR_POLL_US;
            }
        }
    }
  g_debug ("Session thread ending, cleaning up");
  
  if (session->graphics_pipeline && session->surface_created)
    {
      mrd_rdp_graphics_pipeline_delete_surface (session->graphics_pipeline,
                                                 session->surface_id);
      session->surface_created = FALSE;
    }
  if (session->graphics_pipeline)
    {
      mrd_rdp_graphics_pipeline_stop (session->graphics_pipeline);
      g_clear_object (&session->graphics_pipeline);
    }
  if (session->encode_session)
    mrd_encode_session_stop (session->encode_session);

  if (session->input_injector)
    mrd_input_injector_release_modifiers (session->input_injector);
  g_clear_object (&session->screen_capture);
  g_clear_object (&session->input_injector);
  g_clear_object (&session->virtual_display);
  if (no_sleep_assertion != kIOPMNullAssertionID)
    IOPMAssertionRelease (no_sleep_assertion);
  g_message ("Session resources released");
  return NULL;
}

static gboolean
configure_rdp_settings (MrdSessionRdp  *session,
                        GError        **error)
{
  freerdp_peer *peer = session->peer;
  rdpSettings *settings = peer->context->settings;
  
  if (!freerdp_settings_set_bool (settings, FreeRDP_SupportGraphicsPipeline, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_GfxH264, FALSE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444, FALSE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444v2, FALSE))
    goto settings_error;
  
  if (!freerdp_settings_set_bool (settings, FreeRDP_RemoteFxCodec, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_NSCodec, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_FrameMarkerCommandEnabled, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_FastPathOutput, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_RefreshRect, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_SuppressOutput, TRUE))
    goto settings_error;
  
  if (!freerdp_settings_set_bool (settings, FreeRDP_NlaSecurity, FALSE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_TlsSecurity, TRUE))
    goto settings_error;
  if (!freerdp_settings_set_bool (settings, FreeRDP_RdpSecurity, TRUE))
    goto settings_error;
  
  if (session->cert_file && session->key_file)
    {
      rdpCertificate *cert = freerdp_certificate_new_from_file (session->cert_file);
      rdpPrivateKey *key = freerdp_key_new_from_file (session->key_file);
      if (!cert || !key)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to load certificate or key file");
          if (cert)
            freerdp_certificate_free (cert);
          if (key)
            freerdp_key_free (key);
          return FALSE;
        }
      if (!freerdp_settings_set_pointer_len (settings, FreeRDP_RdpServerCertificate, cert, 1))
        goto settings_error;
      if (!freerdp_settings_set_pointer_len (settings, FreeRDP_RdpServerRsaKey, key, 1))
        goto settings_error;
    }
  
  if (!freerdp_settings_set_uint32 (settings, FreeRDP_ColorDepth, 32))
    goto settings_error;
  
  if (!freerdp_settings_set_bool (settings, FreeRDP_SupportDynamicTimeZone, FALSE))
    goto settings_error;
  return TRUE;
settings_error:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Failed to configure RDP settings");
  return FALSE;
}

MrdSessionRdp *
mrd_session_rdp_new (MrdRdpServer   *server,
                     freerdp_peer   *peer,
                     const char     *cert_file,
                     const char     *key_file,
                     GError        **error)
{
  MrdSessionRdp *session;
  RdpPeerContext *peer_ctx;
  session = g_object_new (MRD_TYPE_SESSION_RDP, NULL);
  session->server = server;
  session->peer = peer;
  session->cert_file = g_strdup (cert_file);
  session->key_file = g_strdup (key_file);
  
  peer->ContextSize = sizeof (RdpPeerContext);
  peer->ContextNew = (psPeerContextNew) peer_context_new;
  peer->ContextFree = (psPeerContextFree) peer_context_free;
  if (!freerdp_peer_context_new (peer))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create peer context");
      g_object_unref (session);
      return NULL;
    }
  peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  peer_ctx->session_rdp = session;
  
  if (!configure_rdp_settings (session, error))
    {
      g_object_unref (session);
      return NULL;
    }
  
  peer->Capabilities = on_peer_capabilities;
  peer->PostConnect = on_peer_post_connect;
  peer->Activate = on_peer_activate;
  peer->context->input->KeyboardEvent = on_keyboard_event;
  peer->context->input->MouseEvent = on_mouse_event;
  peer->context->input->ExtendedMouseEvent = on_extended_mouse_event;
  
  session->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  return session;
}
void
mrd_session_rdp_start (MrdSessionRdp *session)
{
  g_return_if_fail (MRD_IS_SESSION_RDP (session));
  if (!session->peer->Initialize (session->peer))
    {
      g_warning ("Failed to initialize peer");
      return;
    }
  session->session_thread = g_thread_new ("rdp-session",
                                          session_thread_func,
                                          session);
}
void
mrd_session_rdp_stop (MrdSessionRdp *session)
{
  g_return_if_fail (MRD_IS_SESSION_RDP (session));
  session->session_should_stop = TRUE;
  if (session->stop_event)
    SetEvent (session->stop_event);
  if (session->session_thread)
    {
      g_thread_join (session->session_thread);
      session->session_thread = NULL;
    }
}
MrdRdpGraphicsPipeline *
mrd_session_rdp_get_graphics_pipeline (MrdSessionRdp *session)
{
  g_return_val_if_fail (MRD_IS_SESSION_RDP (session), NULL);
  return session->graphics_pipeline;
}

static void
mrd_session_rdp_finalize (GObject *object)
{
  MrdSessionRdp *session = MRD_SESSION_RDP (object);
  mrd_session_rdp_stop (session);
  
  if (session->graphics_pipeline && session->surface_created)
    {
      mrd_rdp_graphics_pipeline_delete_surface (session->graphics_pipeline,
                                                 session->surface_id);
    }
  
  if (session->graphics_pipeline)
    {
      mrd_rdp_graphics_pipeline_stop (session->graphics_pipeline);
      g_clear_object (&session->graphics_pipeline);
    }
  g_clear_object (&session->screen_capture);
  g_clear_object (&session->input_injector);
  g_clear_object (&session->encode_session);
  g_clear_object (&session->virtual_display);
  
  g_free (session->prev_frame_buffer);
  session->prev_frame_buffer = NULL;
  
  g_free (session->last_cursor.bitmap);
  session->last_cursor.bitmap = NULL;
  
  session->vcm = NULL;
  if (session->stop_event)
    CloseHandle (session->stop_event);
  if (session->peer)
    {
      freerdp_peer_context_free (session->peer);
      freerdp_peer_free (session->peer);
    }
  g_free (session->cert_file);
  g_free (session->key_file);
  G_OBJECT_CLASS (mrd_session_rdp_parent_class)->finalize (object);
}
static void
mrd_session_rdp_init (MrdSessionRdp *session)
{
}
static void
mrd_session_rdp_class_init (MrdSessionRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_session_rdp_finalize;
}