#include "mrd-session-rdp-private.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "../platform/mrd-virtual-display.h"
#include "../platform/mrd-screen-capture.h"
#include "../platform/mrd-input-injector.h"
#include "../encoding/mrd-encode-session.h"

BOOL
mrd_session_on_peer_capabilities (freerdp_peer *peer)
{
  rdpSettings *settings = peer->context->settings;

  gboolean rfx = freerdp_settings_get_bool (settings, FreeRDP_RemoteFxCodec);
  gboolean nsc = freerdp_settings_get_bool (settings, FreeRDP_NSCodec);
  gboolean gfx = freerdp_settings_get_bool (settings, FreeRDP_SupportGraphicsPipeline);

  /* AVC420/444 caps come later via RDPGFX CapsAdvertise. */
  g_message ("Client capabilities: RFX=%s, NSC=%s, GFX=%s",
             rfx ? "yes" : "no",
             nsc ? "yes" : "no",
             gfx ? "yes" : "no");

  return TRUE;
}

/* 200..300 range = non-HiDPI at (scale-100)% (e.g. /scale-desktop:250). */
void
mrd_session_compute_scale_mode (uint32_t  raw_scale,
                                uint32_t *out_actual_scale,
                                gboolean *out_use_hidpi)
{
  if (raw_scale > 200 && raw_scale <= 300)
    {
      *out_actual_scale = raw_scale - 100;
      *out_use_hidpi = FALSE;
    }
  else if (raw_scale > 100 && raw_scale <= 200)
    {
      *out_actual_scale = raw_scale;
      *out_use_hidpi = TRUE;
    }
  else
    {
      *out_actual_scale = 100;
      *out_use_hidpi = FALSE;
    }
}

MrdVirtualDisplay *
mrd_session_create_vd_for (uint32_t   client_w,
                           uint32_t   client_h,
                           uint32_t   actual_scale,
                           gboolean   use_hidpi,
                           GError   **error)
{
  if (actual_scale > 100)
    {
      uint32_t logical_w = (client_w * 100) / actual_scale;
      uint32_t logical_h = (client_h * 100) / actual_scale;
      logical_w = (logical_w + 1) & ~1u;
      logical_h = (logical_h + 1) & ~1u;

      g_message ("Scaled VD: %ux%u at %u%% → %ux%u logical%s",
                 client_w, client_h, actual_scale, logical_w, logical_h,
                 use_hidpi ? " (HiDPI)" : "");

      if (use_hidpi)
        return mrd_virtual_display_new_hidpi (logical_w, logical_h, 60, error);
      return mrd_virtual_display_new (logical_w, logical_h, 60, error);
    }
  if (use_hidpi)
    return mrd_virtual_display_new_hidpi (client_w, client_h, 60, error);
  return mrd_virtual_display_new (client_w, client_h, 60, error);
}

/* Default: mirror-primary; MRD_DISABLE_MIRROR_PRIMARY → extended-side. */
void
mrd_session_apply_vd_placement (MrdVirtualDisplay *vd)
{
  const char *no_mirror = g_getenv ("MRD_DISABLE_MIRROR_PRIMARY");
  gboolean skip = no_mirror && no_mirror[0] != '\0' &&
                  g_strcmp0 (no_mirror, "0") != 0;
  if (skip)
    {
      const char *side_env = g_getenv ("MRD_VD_EXTEND_SIDE");
      MrdVdSide side = MRD_VD_SIDE_RIGHT;
      if (side_env && g_ascii_strcasecmp (side_env, "left") == 0)
        side = MRD_VD_SIDE_LEFT;
      mrd_virtual_display_place_extended (vd, side);
    }
  else
    {
      mrd_virtual_display_make_primary (vd);
      mrd_virtual_display_mirror_physical (vd);
    }
}

BOOL
mrd_session_on_peer_post_connect (freerdp_peer *peer)
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

  /* Many clients leave 100 and send DISP MonitorLayout later → triggers
   * a one-time HiDPI flip + recreate. /scale-desktop:N skips the recreate. */
  uint32_t scale = 100;
  uint32_t client_scale = freerdp_settings_get_uint32 (settings, FreeRDP_DesktopScaleFactor);
  if (client_scale > 100 && client_scale <= 500)
    {
      scale = client_scale;
      g_message ("Client scale hint: %u%%", scale);
    }

  uint32_t actual_scale = 100;
  gboolean use_hidpi = FALSE;
  mrd_session_compute_scale_mode (scale, &actual_scale, &use_hidpi);

  /* execute_resize needs these if a DISP PDU arrives with scale=0. */
  session->client_width = client_width;
  session->client_height = client_height;
  session->client_scale_percent = scale;

  session->virtual_display = mrd_session_create_vd_for (client_width, client_height,
                                                        actual_scale, use_hidpi, &error);
  if (!session->virtual_display)
    {
      g_warning ("Failed to create virtual display: %s", error->message);
      return FALSE;
    }

  uint32_t vd_id = mrd_virtual_display_get_id (session->virtual_display);

  mrd_session_apply_vd_placement (session->virtual_display);

  uint32_t vd_w = 0, vd_h = 0;
  mrd_virtual_display_get_logical_size (session->virtual_display, &vd_w, &vd_h);
  gboolean is_hidpi = mrd_virtual_display_is_hidpi (session->virtual_display);
  gboolean needs_scaling = (vd_w != client_width || vd_h != client_height) || is_hidpi;

  /* Cursor captured at backing res; HiDPI = 2x. */
  if (is_hidpi)
    session->cursor_scale = (float)client_width / (float)(vd_w * 2);
  else if (needs_scaling)
    session->cursor_scale = (float)client_width / (float)vd_w;
  else
    session->cursor_scale = 1.0f;

  if (session->cursor_scale != 1.0f)
    g_message ("Cursor scale: %.2f", session->cursor_scale);

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

  /* AVC caps determined later via RDPGFX negotiation. */
  session->encode_session = mrd_encode_session_new (FALSE, FALSE);

  session->vcm = peer_ctx->vcm;

  return TRUE;
}

BOOL
mrd_session_on_peer_activate (freerdp_peer *peer)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;
  rdpSettings *settings = peer->context->settings;

  g_message ("Peer activated");

  if (session->input_injector)
    mrd_input_injector_release_modifiers (session->input_injector);

  /* GFX/H.264-only — NV12 zero-copy → VT. */
  gboolean gfx_expected = freerdp_settings_get_bool (settings, FreeRDP_SupportGraphicsPipeline);
  if (!gfx_expected)
    {
      g_warning ("Client doesn't support RDPGFX (H.264) — cannot stream.");
      peer_ctx->codec = MRD_CODEC_NONE;
      return FALSE;
    }

  peer_ctx->codec = MRD_CODEC_NONE;
  g_message ("Waiting for RDPGFX pipeline (GFX-only mode)");

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

  peer_ctx->activated = TRUE;

  /* MRD_CURSOR_IN_VIDEO=1 → cursor is in H.264; disable client overlay
   * (cursor_initialized gates all PointerNew/Cached/Position sites). */
  {
    const char *in_video = g_getenv ("MRD_CURSOR_IN_VIDEO");
    gboolean overlay = !(in_video && in_video[0] != '\0' &&
                         g_strcmp0 (in_video, "0") != 0);
    session->cursor_initialized = overlay;
    session->cursor_cache_index = 0;
    session->last_cursor_seed = -1;
    if (overlay)
      {
        /* Seed from host so first poll doesn't fire a bogus delta-from-(0,0). */
        guint16 sx = 0, sy = 0;
        if (mrd_input_injector_get_client_cursor_position (session->input_injector,
                                                           &sx, &sy))
          {
            session->cursor_x = sx;
            session->cursor_y = sy;
          }
        mrd_session_update_cursor_if_changed (session);
      }
    else
      g_message ("Cursor overlay path disabled (cursor baked into video stream)");
  }

  return TRUE;
}

BOOL
mrd_session_on_keyboard_event (rdpInput         *input,
                               UINT16            flags,
                               UINT8             code)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;

  if (session->input_injector)
    mrd_input_injector_handle_keyboard (session->input_injector, flags, code);

  return TRUE;
}

BOOL
mrd_session_on_mouse_event (rdpInput        *input,
                            UINT16           flags,
                            UINT16           x,
                            UINT16           y)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;

  if (session->input_injector)
    mrd_input_injector_handle_mouse (session->input_injector, flags, x, y);

  /* Update deadband ref. Don't echo PointerPosition — client already drew it. */
  session->cursor_x = x;
  session->cursor_y = y;

  return TRUE;
}

BOOL
mrd_session_on_extended_mouse_event (rdpInput        *input,
                                     UINT16           flags,
                                     UINT16           x,
                                     UINT16           y)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (input->context);
  MrdSessionRdp *session = peer_ctx->session_rdp;

  if (session->input_injector)
    mrd_input_injector_handle_extended_mouse (session->input_injector, flags, x, y);

  /* See on_mouse_event. */
  session->cursor_x = x;
  session->cursor_y = y;

  return TRUE;
}
