#include "mrd-session-rdp.h"
#include "mrd-session-rdp-private.h"

#include <gio/gio.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/string.h>

#include "../mrd-rdp-private.h"
#include "mrd-rdp-server.h"
#include "mrd-rdp-graphics-pipeline.h"
#include "mrd-rdp-disp.h"
#include "../platform/mrd-screen-capture.h"
#include "../platform/mrd-input-injector.h"
#include "../platform/mrd-virtual-display.h"
#include "../encoding/mrd-encode-session.h"
#include "mrd-rdp-clipboard.h"
#include "mrd-rdp-audio.h"
#include "../util/mrd-auth.h"

#include <IOKit/pwr_mgt/IOPMLib.h>

/* Cap on GFX frames in flight: max fps = window / RTT. */
int
mrd_gfx_max_in_flight (void)
{
  static gsize init = 0;
  static int cached = MRD_GFX_MAX_IN_FLIGHT_DEFAULT;

  if (g_once_init_enter (&init))
    {
      const char *env = g_getenv ("MRD_GFX_MAX_IN_FLIGHT");
      if (env && env[0])
        {
          char *end = NULL;
          long v = strtol (env, &end, 10);
          if (end && *end == '\0' && v >= 1 && v <= 32)
            {
              cached = (int) v;
              g_message ("MRD_GFX_MAX_IN_FLIGHT=%d (override)", cached);
            }
          else
            {
              g_warning ("MRD_GFX_MAX_IN_FLIGHT=%s invalid (expected 1..32), "
                         "using default %d", env, cached);
            }
        }
      g_once_init_leave (&init, 1);
    }

  return cached;
}

G_DEFINE_TYPE (MrdSessionRdp, mrd_session_rdp, G_TYPE_OBJECT)

static void
audio_from_capture_trampoline (const int16_t *frames,
                               size_t         n_frames,
                               void          *user_data)
{
  MrdRdpAudio *audio = user_data;
  if (audio)
    mrd_rdp_audio_push_pcm (audio, frames, n_frames);
}

static void
session_refresh_audio_hook (MrdSessionRdp *session)
{
  if (!session->screen_capture)
    return;
  if (session->audio)
    mrd_screen_capture_set_audio_callback (session->screen_capture,
                                           audio_from_capture_trampoline,
                                           session->audio);
  else
    mrd_screen_capture_set_audio_callback (session->screen_capture, NULL, NULL);
}

static BOOL
peer_context_new (freerdp_peer   *peer,
                  RdpPeerContext *peer_ctx)
{
  /* Must precede PostConnect so DVC setup runs during handshake. */
  peer_ctx->vcm = WTSOpenServerA ((LPSTR) peer->context);
  if (!peer_ctx->vcm || peer_ctx->vcm == INVALID_HANDLE_VALUE)
    {
      g_warning ("Failed to open virtual channel manager");
      return FALSE;
    }

  return TRUE;
}

static void
peer_context_free (freerdp_peer   *peer,
                   RdpPeerContext *peer_ctx)
{
  (void) peer;

  if (peer_ctx->vcm && peer_ctx->vcm != INVALID_HANDLE_VALUE)
    {
      WTSCloseServer (peer_ctx->vcm);
      peer_ctx->vcm = NULL;
    }
}

/* Runs on FreeRDP DISP DVC thread — only stashes; session thread mutates. */
void
mrd_session_rdp_request_resize (MrdSessionRdp *session,
                                uint32_t       width,
                                uint32_t       height,
                                uint32_t       scale_percent)
{
  g_return_if_fail (MRD_IS_SESSION_RDP (session));

  uint32_t w = CLAMP (width, MIN_VD_WIDTH, MAX_VD_WIDTH);
  uint32_t h = CLAMP (height, MIN_VD_HEIGHT, MAX_VD_HEIGHT);
  w = (w + 1u) & ~1u;
  h = (h + 1u) & ~1u;

  uint32_t s = scale_percent == 0 ? 100 : scale_percent;

  g_mutex_lock (&session->resize_mutex);
  /* Drop no-op layouts (client re-announces on channel open). */
  if (!session->resize_pending &&
      w == session->client_width &&
      h == session->client_height &&
      s == session->client_scale_percent)
    {
      g_mutex_unlock (&session->resize_mutex);
      return;
    }

  session->pending_width = w;
  session->pending_height = h;
  session->pending_scale = s;
  session->resize_pending = TRUE;
  g_mutex_unlock (&session->resize_mutex);

  if (session->resize_event)
    SetEvent (session->resize_event);

  g_message ("[DISP] queued resize: %ux%u @ %u%%", w, h, s);
}

/* Session thread, pipeline-drained by caller. FALSE → close peer. */
static gboolean
execute_resize (MrdSessionRdp *session,
                uint32_t       new_w,
                uint32_t       new_h,
                uint32_t       new_scale)
{
  freerdp_peer *peer = session->peer;
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
  g_autoptr (GError) error = NULL;

  g_message ("[DISP] execute_resize → %ux%u @ %u%%",
             new_w, new_h, new_scale);

  session->surface_created = FALSE;   /* freeze submission */

  if (session->encode_session)
    {
      GQueue ready = G_QUEUE_INIT;
      while (mrd_encode_session_drain_ready (session->encode_session,
                                             &ready) > 0)
        {
          MrdEncodedFrame *ef;
          while ((ef = g_queue_pop_head (&ready)) != NULL)
            mrd_encode_session_release_frame (session->encode_session, ef);
        }
      mrd_encode_session_stop (session->encode_session);
      g_clear_object (&session->encode_session);
    }

  if (session->screen_capture)
    {
      mrd_screen_capture_stop (session->screen_capture);
      g_clear_object (&session->screen_capture);
    }

  /* Keep the GFX pipeline; just drop the surface. */
  if (session->graphics_pipeline)
    {
      mrd_rdp_graphics_pipeline_delete_surface (session->graphics_pipeline,
                                                session->surface_id);
    }

  /* In-place applySettings: preserves displayID + placement.
   * Falls through to recreate for HiDPI flip, envelope, MRD_REPLACE_VD=1. */
  uint32_t actual_scale = 100;
  gboolean use_hidpi = FALSE;
  mrd_session_compute_scale_mode (new_scale, &actual_scale, &use_hidpi);

  uint32_t new_logical_w = new_w;
  uint32_t new_logical_h = new_h;
  if (actual_scale > 100)
    {
      new_logical_w = (new_w * 100) / actual_scale;
      new_logical_h = (new_h * 100) / actual_scale;
      new_logical_w = (new_logical_w + 1u) & ~1u;
      new_logical_h = (new_logical_h + 1u) & ~1u;
    }

  gboolean inplace_done = FALSE;
  {
    const char *replace_env = g_getenv ("MRD_REPLACE_VD");
    gboolean force_replace = replace_env && replace_env[0] != '\0' &&
                             g_strcmp0 (replace_env, "0") != 0;
    if (!force_replace && session->virtual_display)
      {
        g_autoptr (GError) recfg_err = NULL;
        if (mrd_virtual_display_reconfigure (session->virtual_display,
                                             new_logical_w, new_logical_h,
                                             60, use_hidpi, &recfg_err))
          {
            inplace_done = TRUE;
          }
        else
          {
            g_message ("[DISP] inplace declined → recreate (%s)",
                       recfg_err ? recfg_err->message : "unknown");
          }
      }
  }

  if (!inplace_done)
    {
      g_clear_object (&session->virtual_display);

      session->virtual_display = mrd_session_create_vd_for (new_w, new_h, actual_scale,
                                                            use_hidpi, &error);
      if (!session->virtual_display)
        {
          g_warning ("[DISP] VD rebuild failed: %s",
                     error ? error->message : "unknown");
          return FALSE;
        }

      mrd_session_apply_vd_placement (session->virtual_display);
    }

  uint32_t vd_id = mrd_virtual_display_get_id (session->virtual_display);

  /* 9. Rebuild capture with the same scaling decision as connect. */
  uint32_t vd_w = 0, vd_h = 0;
  mrd_virtual_display_get_logical_size (session->virtual_display, &vd_w, &vd_h);
  gboolean is_hidpi = mrd_virtual_display_is_hidpi (session->virtual_display);
  gboolean needs_scaling = (vd_w != new_w || vd_h != new_h) || is_hidpi;

  if (is_hidpi)
    session->cursor_scale = (float) new_w / (float) (vd_w * 2);
  else if (needs_scaling)
    session->cursor_scale = (float) new_w / (float) vd_w;
  else
    session->cursor_scale = 1.0f;

  if (needs_scaling)
    {
      session->screen_capture = mrd_screen_capture_new_scaled (vd_id, new_w,
                                                               new_h);
      g_message ("Screen capture: scaling to %ux%u", new_w, new_h);
    }
  else
    {
      session->screen_capture = mrd_screen_capture_new_for_display (vd_id);
    }

  g_autoptr (GError) cap_err = NULL;
  if (!mrd_screen_capture_start (session->screen_capture, &cap_err))
    {
      g_warning ("[DISP] Screen capture restart failed: %s",
                 cap_err ? cap_err->message : "unknown");
      return FALSE;
    }

  /* Audio DVC is long-lived — re-hook the new capture into it. */
  session_refresh_audio_hook (session);

  if (session->input_injector)
    {
      mrd_input_injector_set_target_display (session->input_injector, vd_id);
      mrd_input_injector_set_client_size (session->input_injector,
                                          new_w, new_h);
    }

  session->client_width = new_w;
  session->client_height = new_h;
  session->client_scale_percent = new_scale;

  /* get_dimensions reports actual output dims after the scaler. */
  int cap_width = (int) new_w;
  int cap_height = (int) new_h;
  mrd_screen_capture_get_dimensions (session->screen_capture,
                                     &cap_width, &cap_height, NULL);

  gboolean have_avc444 = FALSE, have_avc420 = FALSE;
  if (session->graphics_pipeline)
    mrd_rdp_graphics_pipeline_get_capabilities (session->graphics_pipeline,
                                                &have_avc444, &have_avc420);

  session->encode_session = mrd_encode_session_new (have_avc444, have_avc420);
  g_autoptr (GError) enc_err = NULL;
  if (!mrd_encode_session_start (session->encode_session,
                                 (uint32_t) cap_width, (uint32_t) cap_height,
                                 &enc_err))
    {
      g_warning ("[DISP] encoder restart failed: %s",
                 enc_err ? enc_err->message : "unknown");
      return FALSE;
    }

  if (session->graphics_pipeline)
    mrd_rdp_graphics_pipeline_send_reset_graphics (session->graphics_pipeline,
                                                    (uint32_t) cap_width,
                                                    (uint32_t) cap_height);

  g_autoptr (GError) surf_err = NULL;
  if (session->graphics_pipeline &&
      !mrd_rdp_graphics_pipeline_create_surface (session->graphics_pipeline,
                                                  0, (uint32_t) cap_width,
                                                  (uint32_t) cap_height,
                                                  &surf_err))
    {
      g_warning ("[DISP] surface rebuild failed: %s",
                 surf_err ? surf_err->message : "unknown");
      return FALSE;
    }

  session->surface_id = 0;

  /* Stale after ResetGraphics. */
  g_atomic_int_set (&session->frames_in_flight, 0);

  g_autoptr (GError) nv12_err = NULL;
  if (!mrd_screen_capture_enable_nv12 (session->screen_capture, &nv12_err))
    g_warning ("[DISP] NV12 switch failed: %s",
               nv12_err ? nv12_err->message : "unknown");

  /* Cursor cache invalidated by VD rebuild. */
  g_free (session->last_cursor.bitmap);
  memset (&session->last_cursor, 0, sizeof session->last_cursor);
  session->cursor_cache_index = 0;
  session->last_cursor_seed = -1;

  session->surface_created = TRUE;
  peer_ctx->codec = MRD_CODEC_GFX;

  g_message ("[DISP] resize complete (%dx%d, cursor_scale %.2f, %s)",
             cap_width, cap_height, session->cursor_scale,
             inplace_done ? "inplace" : "recreated");
  return TRUE;
}

/* Main-thread idle: unref-from-session-thread would deadlock (finalize→join). */
static gboolean
remove_session_idle (gpointer user_data)
{
  MrdSessionRdp *session = MRD_SESSION_RDP (user_data);
  if (session->server)
    mrd_rdp_server_remove_session (session->server, session);
  return G_SOURCE_REMOVE;
}

static gpointer
session_thread_func (gpointer data)
{
  MrdSessionRdp *session = MRD_SESSION_RDP (data);
  freerdp_peer *peer = session->peer;
  HANDLE events[MAXIMUM_WAIT_OBJECTS];
  DWORD event_count;
  const DWORD events_cap = (DWORD) (sizeof (events) / sizeof (events[0]));

  g_debug ("Session thread started");

  /* Without this, macOS sleeps the VD and SCK stops delivering frames. */
  IOPMAssertionID no_sleep_assertion = kIOPMNullAssertionID;
  IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleDisplaySleep,
                               kIOPMAssertionLevelOn,
                               CFSTR ("RDP client connected"),
                               &no_sleep_assertion);

  while (!session->session_should_stop)
    {
      event_count = 0;

      #define MRD_PUSH_EVENT(h) G_STMT_START { \
        if (event_count >= events_cap) { \
          g_warning ("Session event slots exhausted (%u), dropping wakeup source", \
                     (unsigned)events_cap); \
        } else { \
          events[event_count++] = (h); \
        } \
      } G_STMT_END

      MRD_PUSH_EVENT (session->stop_event);

      DWORD peer_room = events_cap - event_count;
      DWORD peer_request = peer_room < 32 ? peer_room : 32;
      DWORD peer_count = peer->GetEventHandles (peer, &events[event_count], peer_request);
      if (peer_count == 0)
        {
          g_usleep (10000);  /* 10ms */
          continue;
        }
      event_count += peer_count;

      if (session->vcm)
        {
          HANDLE vcm_event = WTSVirtualChannelManagerGetEventHandle (session->vcm);
          if (vcm_event)
            MRD_PUSH_EVENT (vcm_event);
        }

      HANDLE gfx_event_handle = NULL;
      if (session->graphics_pipeline)
        {
          gfx_event_handle = mrd_rdp_graphics_pipeline_get_event_handle (session->graphics_pipeline);
          if (gfx_event_handle)
            MRD_PUSH_EVENT (gfx_event_handle);
        }

      HANDLE enc_event_handle = NULL;
      if (session->encode_session && session->surface_created)
        {
          enc_event_handle = mrd_encode_session_get_output_event_handle (session->encode_session);
          if (enc_event_handle)
            MRD_PUSH_EVENT (enc_event_handle);
        }

      HANDLE frame_event_handle = NULL;
      if (session->screen_capture && session->surface_created)
        {
          frame_event_handle = mrd_screen_capture_get_frame_event_handle (session->screen_capture);
          if (frame_event_handle)
            MRD_PUSH_EVENT (frame_event_handle);
        }

      if (session->resize_event)
        MRD_PUSH_EVENT (session->resize_event);

      HANDLE audio_event = NULL;
      if (session->audio)
        {
          audio_event = mrd_rdp_audio_get_event_handle (session->audio);
          if (audio_event)
            MRD_PUSH_EVENT (audio_event);
        }

      #undef MRD_PUSH_EVENT

      /* Event-driven. Cursor overlay needs wakeups even on still frames. */
      RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (peer->context);
      DWORD wait_ms = (peer_ctx->activated && session->cursor_initialized)
                      ? MRD_CURSOR_POS_POLL_MS : 100;

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
                          /* NV12-only build: no RFX fallback possible. */
                          g_warning ("Failed to open RDPGFX channel — disconnecting peer");
                          g_clear_object (&session->graphics_pipeline);
                          peer->Close (peer);
                          break;
                        }
                    }

                  /* CLIPRDR runs its own internal thread. */
                  session->clipboard = mrd_rdp_clipboard_new (session, session->vcm);
                  if (session->clipboard &&
                      !mrd_rdp_clipboard_start (session->clipboard))
                    {
                      g_warning ("CLIPRDR start failed; clipboard disabled");
                      g_clear_object (&session->clipboard);
                    }
                  session->next_clipboard_poll_us =
                    g_get_monotonic_time () + MRD_CLIPBOARD_POLL_US;

                  session->audio = mrd_rdp_audio_new (session, session->vcm);
                  if (session->audio &&
                      !mrd_rdp_audio_start (session->audio))
                    {
                      g_warning ("rdpsnd start failed; audio disabled");
                      g_clear_object (&session->audio);
                    }
                  session_refresh_audio_hook (session);

                  /* DISP last (depends on DRDYNVC). MRD_DISABLE_DISP=1 skips. */
                  {
                    rdpSettings *settings = peer->context->settings;
                    const char *disable_env = g_getenv ("MRD_DISABLE_DISP");
                    gboolean disable = disable_env && disable_env[0] != '\0' &&
                                       g_strcmp0 (disable_env, "0") != 0;
                    gboolean client_ok = freerdp_settings_get_bool (
                      settings, FreeRDP_SupportDisplayControl);
                    if (disable)
                      {
                        g_message ("MRD_DISABLE_DISP set — skipping DISP DVC");
                      }
                    else if (!client_ok)
                      {
                        g_message ("Client did not advertise DisplayControl — "
                                   "skipping DISP DVC");
                      }
                    else
                      {
                        session->disp = mrd_rdp_disp_new (session, session->vcm);
                        if (session->disp &&
                            !mrd_rdp_disp_open (session->disp))
                          {
                            g_warning ("DISP open failed; dynamic resize disabled");
                            g_clear_object (&session->disp);
                          }
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
                  /* NV12-only build: no RFX fallback possible. */
                  g_warning ("RDPGFX message handling failed — disconnecting peer");
                  if (session->surface_created)
                    session->surface_created = FALSE;
                  mrd_rdp_graphics_pipeline_stop (session->graphics_pipeline);
                  g_clear_object (&session->graphics_pipeline);
                  if (session->encode_session)
                    mrd_encode_session_stop (session->encode_session);
                  peer->Close (peer);
                  break;
                }
            }
        }

      /* MUST be outside handle_messages — concurrent DVC write corrupts state.
       * Skip rest of iteration so client processes reset before next frame. */
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
              session->surface_created = TRUE;
              peer_ctx->codec = MRD_CODEC_GFX;

              g_atomic_int_set (&session->frames_in_flight, 0);
              mrd_rdp_graphics_pipeline_set_frame_ack_callback (
                session->graphics_pipeline, mrd_session_on_gfx_frame_ack, session);

              /* MRD_ADAPTIVE=0 disables. floor=2 Mbps; ceiling=MRD_BITRATE_MBPS. */
              {
                const char *adaptive_env = g_getenv ("MRD_ADAPTIVE");
                gboolean adaptive_on = !(adaptive_env && g_strcmp0 (adaptive_env, "0") == 0);
                int ceiling = mrd_encode_session_get_initial_bitrate_mbps ();
                int floor_mbps = 2;
                if (ceiling <= 0)
                  adaptive_on = FALSE;
                if (adaptive_on && floor_mbps > ceiling)
                  floor_mbps = ceiling;

                mrd_rdp_graphics_pipeline_set_bitrate_callback (
                  session->graphics_pipeline, mrd_session_on_gfx_bitrate_change, session);
                mrd_rdp_graphics_pipeline_configure_adaptive (
                  session->graphics_pipeline,
                  adaptive_on, ceiling, floor_mbps, ceiling,
                  (guint) mrd_gfx_max_in_flight () + 2);
              }

              /* IOSurface-backed NV12 → straight to VT. */
              g_autoptr(GError) nv12_err = NULL;
              if (!mrd_screen_capture_enable_nv12 (session->screen_capture,
                                                   &nv12_err))
                g_warning ("NV12 capture switch failed: %s",
                           nv12_err->message);

              g_message ("H.264 frame streaming started, uncapped (%dx%d)",
                         cap_width, cap_height);
            }
        }

      if (peer_ctx->activated && session->surface_created &&
          peer_ctx->codec == MRD_CODEC_GFX)
        {
          gboolean frame_ready = frame_event_handle &&
            (WaitForSingleObject (frame_event_handle, 0) == WAIT_OBJECT_0);
          mrd_session_pump_frame_gfx (session, frame_ready);
        }

      if (peer_ctx->activated && session->cursor_initialized)
        {
          gint64 now_us = g_get_monotonic_time ();
          if (now_us >= session->next_cursor_check_us)
            {
              mrd_session_update_cursor_if_changed (session);
              session->next_cursor_check_us = now_us + MRD_CURSOR_POLL_US;
            }
          if (now_us >= session->next_cursor_pos_check_us)
            {
              mrd_session_poll_server_cursor_position (session);
              session->next_cursor_pos_check_us = now_us + MRD_CURSOR_POS_POLL_US;
            }
        }

      if (session->clipboard)
        {
          gint64 now_us = g_get_monotonic_time ();
          if (now_us >= session->next_clipboard_poll_us)
            {
              mrd_rdp_clipboard_poll_host (session->clipboard);
              session->next_clipboard_poll_us = now_us + MRD_CLIPBOARD_POLL_US;
            }
        }

      if (session->audio && audio_event &&
          WaitForSingleObject (audio_event, 0) == WAIT_OBJECT_0)
        {
          if (!mrd_rdp_audio_handle_messages (session->audio))
            {
              g_warning ("rdpsnd handle_messages failed; disabling audio");
              mrd_rdp_audio_stop (session->audio);
              g_clear_object (&session->audio);
            }
        }

      if (session->audio)
        mrd_rdp_audio_pump (session->audio);

      /* Drained pipeline + 250ms rate-limit so rapid drags coalesce. */
      if (session->resize_pending && session->surface_created &&
          peer_ctx->codec == MRD_CODEC_GFX && session->graphics_pipeline)
        {
          guint in_flight = (guint) g_atomic_int_get (&session->frames_in_flight);
          guint outstanding = session->encode_session
            ? mrd_encode_session_get_outstanding (session->encode_session)
            : 0;
          gint64 now_us = g_get_monotonic_time ();
          gint64 since = now_us - session->last_resize_us;
          gboolean gate_open = (in_flight == 0 && outstanding == 0 &&
                                since >= MRD_RESIZE_MIN_INTERVAL_US);

          if (gate_open)
            {
              uint32_t w, h, s;
              g_mutex_lock (&session->resize_mutex);
              w = session->pending_width;
              h = session->pending_height;
              s = session->pending_scale;
              session->resize_pending = FALSE;
              if (session->resize_event)
                ResetEvent (session->resize_event);
              g_mutex_unlock (&session->resize_mutex);

              session->last_resize_us = now_us;

              if (!execute_resize (session, w, h, s))
                {
                  g_warning ("[DISP] execute_resize failed — closing peer");
                  peer->Close (peer);
                  break;
                }
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
  if (session->clipboard)
    {
      mrd_rdp_clipboard_stop (session->clipboard);
      g_clear_object (&session->clipboard);
    }
  if (session->disp)
    {
      mrd_rdp_disp_close (session->disp);
      g_clear_object (&session->disp);
    }
  if (session->encode_session)
    mrd_encode_session_stop (session->encode_session);

  /* Release VD; clear modifiers before injector dies. */
  if (session->input_injector)
    mrd_input_injector_release_modifiers (session->input_injector);

  /* Stop capture first so its audio delegate stops dereferencing audio. */
  g_clear_object (&session->screen_capture);
  if (session->audio)
    {
      mrd_rdp_audio_stop (session->audio);
      g_clear_object (&session->audio);
    }
  g_clear_object (&session->input_injector);
  g_clear_object (&session->virtual_display);

  if (no_sleep_assertion != kIOPMNullAssertionID)
    IOPMAssertionRelease (no_sleep_assertion);

  g_message ("Session resources released");

  /* Idle holds its own ref → survives if list-ref drops first on shutdown. */
  if (session->server)
    {
      g_object_ref (session);
      g_idle_add_full (G_PRIORITY_DEFAULT,
                       remove_session_idle,
                       session,
                       g_object_unref);
    }

  return NULL;
}

/* Logon callback: called by FreeRDP with client credentials. Fires in
 * both NLA (from CredSSP) and non-NLA (from the RDP Info PDU) flows, so
 * the bypass check must live here as well as in configure_rdp_settings. */
static BOOL
session_logon (freerdp_peer                  *peer,
               const SEC_WINNT_AUTH_IDENTITY *identity,
               BOOL                           automatic)
{
  (void) automatic;

  MrdSessionRdp *session = MRD_RDP_PEER_CONTEXT (peer->context)->session_rdp;
  if (!session)
    return FALSE;

  const char *require_env = g_getenv ("MRD_REQUIRE_AUTH");
  gboolean require = require_env && require_env[0] != '\0' &&
                     g_strcmp0 (require_env, "0") != 0;

  if (!require)
    {
      g_message ("Logon accepted (auth disabled)");
      return TRUE;
    }

  if (!session->auth || !identity)
    return FALSE;

  char *user = ConvertWCharNToUtf8Alloc ((const WCHAR *) identity->User,
                                         identity->UserLength, NULL);
  char *pass = ConvertWCharNToUtf8Alloc ((const WCHAR *) identity->Password,
                                         identity->PasswordLength, NULL);

  BOOL ok = user && pass && mrd_auth_verify (session->auth, user, pass);

  if (!ok)
    g_warning ("Auth failed for user=%s", user ? user : "(null)");

  if (pass)
    {
      memset (pass, 0, strlen (pass));
      free (pass);
    }
  free (user);
  return ok;
}

static gboolean
configure_rdp_settings (MrdSessionRdp  *session,
                        GError        **error)
{
  freerdp_peer *peer = session->peer;
  rdpSettings *settings = peer->context->settings;

  /* GfxH264/AVC444 stay FALSE — set later during RDPGFX caps negotiation. */
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

  /* Client must opt in (xfreerdp /dynamic-resolution); we just accept. */
  if (!freerdp_settings_set_bool (settings, FreeRDP_SupportDisplayControl, TRUE))
    goto settings_error;

  /* peer->Logon verifies against MrdAuth. */
  if (!freerdp_settings_set_bool (settings, FreeRDP_NlaSecurity, TRUE))
    goto settings_error;
  {
    const char *require_env = g_getenv ("MRD_REQUIRE_AUTH");
    gboolean require = require_env && require_env[0] != '\0' &&
                       g_strcmp0 (require_env, "0") != 0;
    if (!freerdp_settings_set_bool (settings, FreeRDP_NlaSecurity, require))
      goto settings_error;
    if (!require)
      g_warning ("Auth disabled — NLA off, any credentials accepted "
                 "(set MRD_REQUIRE_AUTH=1 to enforce)");
  }
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
                     MrdAuth        *auth,
                     GError        **error)
{
  MrdSessionRdp *session;
  RdpPeerContext *peer_ctx;

  session = g_object_new (MRD_TYPE_SESSION_RDP, NULL);
  session->server = server;
  session->peer = peer;
  session->cert_file = g_strdup (cert_file);
  session->key_file = g_strdup (key_file);
  session->auth = auth;

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

  peer->Capabilities = mrd_session_on_peer_capabilities;
  peer->PostConnect = mrd_session_on_peer_post_connect;
  peer->Activate = mrd_session_on_peer_activate;
  peer->Logon = session_logon;

  peer->context->input->KeyboardEvent = mrd_session_on_keyboard_event;
  peer->context->input->MouseEvent = mrd_session_on_mouse_event;
  peer->context->input->ExtendedMouseEvent = mrd_session_on_extended_mouse_event;

  session->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);

  /* Manual-reset → loop re-checks until pipeline is drained. */
  g_mutex_init (&session->resize_mutex);
  session->resize_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  session->pending_scale = 100;
  session->last_resize_us = 0;

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

  /* Surface delete must precede pipeline stop. */
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

  if (session->clipboard)
    {
      mrd_rdp_clipboard_stop (session->clipboard);
      g_clear_object (&session->clipboard);
    }

  if (session->disp)
    {
      mrd_rdp_disp_close (session->disp);
      g_clear_object (&session->disp);
    }

  /* Capture before audio: delegate may still hold the audio pointer. */
  g_clear_object (&session->screen_capture);
  if (session->audio)
    {
      mrd_rdp_audio_stop (session->audio);
      g_clear_object (&session->audio);
    }
  g_clear_object (&session->input_injector);
  g_clear_object (&session->encode_session);
  g_clear_object (&session->virtual_display);

  g_free (session->last_cursor.bitmap);
  session->last_cursor.bitmap = NULL;

  /* VCM owned by peer_context_free. */
  session->vcm = NULL;

  if (session->stop_event)
    CloseHandle (session->stop_event);

  if (session->resize_event)
    {
      CloseHandle (session->resize_event);
      session->resize_event = NULL;
    }
  g_mutex_clear (&session->resize_mutex);

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
  (void) session;
}

static void
mrd_session_rdp_class_init (MrdSessionRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mrd_session_rdp_finalize;
}
