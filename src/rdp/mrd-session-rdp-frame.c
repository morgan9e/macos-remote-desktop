#include "mrd-session-rdp-private.h"

#include <CoreVideo/CoreVideo.h>
#include <cairo.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "mrd-rdp-graphics-pipeline.h"
#include "../platform/mrd-screen-capture.h"
#include "../encoding/mrd-encode-session.h"
#include "../util/mrd-telemetry.h"

/* Runs on rdpgfx channel thread; flow-control signal. */
void
mrd_session_on_gfx_frame_ack (MrdRdpGraphicsPipeline *pipeline,
                              uint32_t                frame_id,
                              void                   *user_data)
{
  MrdSessionRdp *session = user_data;
  (void)pipeline;
  (void)frame_id;

  g_atomic_int_dec_and_test (&session->frames_in_flight);
}

/* FALSE → controller disables itself for the rest of the session. */
gboolean
mrd_session_on_gfx_bitrate_change (MrdRdpGraphicsPipeline *pipeline,
                                   int                     mbps,
                                   void                   *user_data)
{
  MrdSessionRdp *session = user_data;
  (void)pipeline;

  if (!session->encode_session)
    return FALSE;
  return mrd_encode_session_set_bitrate_mbps (session->encode_session, mbps);
}

/* try_acquire=FALSE → drain only (no new SCK frame). */
void
mrd_session_pump_frame_gfx (MrdSessionRdp *session, gboolean try_acquire)
{
  RdpPeerContext *peer_ctx = MRD_RDP_PEER_CONTEXT (session->peer->context);

  if (!peer_ctx->activated || !session->surface_created)
    return;

  /* 1: drain → submit. */
  GQueue ready = G_QUEUE_INIT;
  mrd_encode_session_drain_ready (session->encode_session, &ready);
  MrdEncodedFrame *ef;
  while ((ef = g_queue_pop_head (&ready)) != NULL)
    {
      g_autoptr(GError) submit_err = NULL;
      if (mrd_rdp_graphics_pipeline_submit_frame (session->graphics_pipeline,
                                                   session->surface_id,
                                                   ef->main_bs, ef->aux_bs,
                                                   ef->damage_region,
                                                   ef->encode_us,
                                                   ef->payload_bytes,
                                                   &submit_err))
        {
          g_atomic_int_inc (&session->frames_in_flight);
        }
      else
        {
          g_warning ("GFX submit failed: %s", submit_err->message);
        }
      mrd_encode_session_release_frame (session->encode_session, ef);
    }

  /* 2: feed encoder one new frame if flow control allows. */
  if (!try_acquire)
    return;

  if (mrd_rdp_graphics_pipeline_acks_suspended (session->graphics_pipeline))
    {
      session->gfx_skipped_count++;
      return;
    }

  /* Pipeline must drain to 0 before resize can run. */
  if (session->resize_pending)
    {
      session->gfx_skipped_count++;
      return;
    }

  /* outstanding = submitted-to-VT-but-not-
   * drained plus completed-but-not-yet-submitted; summed with in_flight
   * it's the total pipeline occupancy from capture to client ack. */
  guint in_flight   = (guint) g_atomic_int_get (&session->frames_in_flight);
  guint outstanding = mrd_encode_session_get_outstanding (session->encode_session);
  guint target      = (guint) mrd_gfx_max_in_flight () + 2;
  if (in_flight + outstanding >= target)
    {
      session->gfx_skipped_count++;
      return;
    }

  MrdCapturedFrame *cf =
    mrd_screen_capture_take_captured_frame (session->screen_capture);
  if (!cf)
    return;  /* No frame ready — normal, not an error */

  CVPixelBufferRef pb = (CVPixelBufferRef)cf->pixel_buffer;

  cairo_region_t *damage_region = cf->dirty;
  cf->dirty = NULL;
  if (!damage_region || cairo_region_num_rectangles (damage_region) == 0)
    {
      if (damage_region)
        cairo_region_destroy (damage_region);
      mrd_captured_frame_free (cf);
      session->gfx_skipped_count++;
      return;
    }

  /* Pre-submit sample so ratio reflects backlog before this frame. */
  mrd_rdp_graphics_pipeline_record_occupancy (session->graphics_pipeline,
                                              in_flight + outstanding);

  /* damage_region ownership transfers. */
  g_autoptr(GError) sub_err = NULL;
  if (!mrd_encode_session_submit_pixel_buffer (session->encode_session,
                                                pb, damage_region,
                                                &sub_err))
    {
      g_warning ("Async encode submit failed: %s", sub_err->message);
    }

  mrd_captured_frame_free (cf);

  session->gfx_frame_count++;
  session->gfx_fps_frame_count++;

  gint64 now = g_get_monotonic_time ();
  if (session->gfx_fps_last_time == 0)
    session->gfx_fps_last_time = now;

  gint64 elapsed = now - session->gfx_fps_last_time;
  if (elapsed >= G_USEC_PER_SEC)
    {
      double fps = (double)session->gfx_fps_frame_count * G_USEC_PER_SEC / elapsed;
      MRD_TELEMETRY_LOG ("FPS: %.1f (frame #%"G_GUINT64_FORMAT", skipped %"G_GUINT64_FORMAT")",
                         fps, session->gfx_frame_count, session->gfx_skipped_count);
      session->gfx_fps_frame_count = 0;
      session->gfx_skipped_count = 0;
      session->gfx_fps_last_time = now;
    }
}
