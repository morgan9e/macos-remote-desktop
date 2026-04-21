#include "mrd-rdp-graphics-pipeline.h"

#include <cairo.h>
#include <gio/gio.h>
#include <freerdp/freerdp.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/color.h>

#include "mrd-session-rdp.h"
#include "../util/mrd-bitstream.h"
#include "../util/mrd-telemetry.h"

#define PROTOCOL_TIMEOUT_MS (10 * 1000)

struct _MrdRdpGraphicsPipeline
{
  GObject parent;

  MrdSessionRdp *session_rdp;
  RdpgfxServerContext *rdpgfx_context;

  gboolean channel_opened;
  gboolean initialized;
  gboolean caps_received;
  gboolean reset_sent;

  gboolean have_avc444;
  gboolean have_avc420;
  uint32_t negotiated_caps_version;
  uint32_t negotiated_caps_flags;

  RFX_CONTEXT *rfx_context;
  wStream *encode_stream;

  GHashTable *surfaces;  /* surface_id → SurfaceInfo* */

  uint32_t next_frame_id;
  GHashTable *pending_frames;  /* frame_id → submit µs */

  /* Rolling stats reset per ack-window (all in lockstep). */
  uint32_t rtt_samples;
  int64_t  rtt_sum_us;
  int64_t  rtt_min_us;
  int64_t  rtt_max_us;
  int64_t  encode_sum_us;
  int64_t  encode_max_us;
  int64_t  send_sum_us;
  int64_t  send_max_us;
  uint64_t bytes_sum;
  uint64_t bytes_max;

  MrdRdpGraphicsPipelineFrameAckCallback frame_ack_callback;
  void *frame_ack_user_data;

  /* Adaptive bitrate: drops fast on bad window, raises slowly after stable. */
  gboolean adaptive_enabled;
  int      adaptive_min_mbps;
  int      adaptive_max_mbps;
  int      adaptive_current_mbps;
  guint    adaptive_target_occupancy;
  int      high_occupancy_windows;
  int      stable_windows;
  int64_t  prev_window_rtt_avg_us;

  uint64_t occupancy_sum;
  uint32_t occupancy_samples;

  MrdRdpGraphicsPipelineBitrateCallback bitrate_callback;
  void *bitrate_user_data;

  /* SUSPEND_FRAME_ACKNOWLEDGEMENT: drain pending_frames + gate submits. */
  gboolean acks_suspended;
  uint32_t last_queue_depth;
  uint32_t last_total_decoded;

  /* Grow-only RDPGFX_AVC420_BITMAP_STREAM.meta backing; reused across frames
   * (incl. AVC444 main+aux — SurfaceCommand serializes meta synchronously). */
  RECTANGLE_16              *meta_rects_buf;
  RDPGFX_H264_QUANT_QUALITY *meta_qq_buf;
  guint                      meta_rects_cap;

  GMutex mutex;
};

#define MRD_GFX_RTT_LOG_WINDOW 60

/* MS-RDPEGFX 2.2.3.13: 0xFFFFFFFF queueDepth = suspend; any other = resume. */
#define MRD_GFX_SUSPEND_QUEUE_DEPTH 0xFFFFFFFFu

G_DEFINE_TYPE (MrdRdpGraphicsPipeline, mrd_rdp_graphics_pipeline, G_TYPE_OBJECT)

typedef struct {
  uint16_t surface_id;
  uint32_t width;
  uint32_t height;
  gboolean mapped_to_output;
} SurfaceInfo;

/* v10+: AVC420 unless AVC_DISABLED. v8.1: requires AVC420_ENABLED. v8: no.
 * Version-max alone is wrong — v10 can still advertise AVC_DISABLED. */
static gboolean
capset_supports_avc420 (uint32_t version, uint32_t flags)
{
  if (version >= RDPGFX_CAPVERSION_10)
    return !(flags & RDPGFX_CAPS_FLAG_AVC_DISABLED);
  if (version == RDPGFX_CAPVERSION_81)
    return (flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) != 0;
  return FALSE;
}

static UINT
rdpgfx_caps_advertise (RdpgfxServerContext             *context,
                       const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise)
{
  MrdRdpGraphicsPipeline *pipeline = context->custom;
  RDPGFX_CAPS_CONFIRM_PDU caps_confirm = {0};
  uint32_t best_avc_version = 0;
  uint32_t best_avc_flags = 0;
  uint32_t fallback_version = 0;
  uint32_t fallback_flags = 0;

  g_mutex_lock (&pipeline->mutex);

  g_message ("RDPGFX: Received caps advertise with %u cap sets",
             caps_advertise->capsSetCount);

  /* Two-pass pick: prefer the highest-version set whose flags actually
   * allow AVC420. If none, ack with the highest seen so the handshake
   * completes; submit_frame will then reject with NOT_SUPPORTED. */
  for (uint16_t i = 0; i < caps_advertise->capsSetCount; i++)
    {
      const RDPGFX_CAPSET *caps = &caps_advertise->capsSets[i];
      gboolean avc_ok = capset_supports_avc420 (caps->version, caps->flags);

      g_message ("  Cap set %u: version 0x%08X, flags 0x%08X, avc420=%s",
                 i, caps->version, caps->flags, avc_ok ? "yes" : "no");

      if (avc_ok && caps->version > best_avc_version)
        {
          best_avc_version = caps->version;
          best_avc_flags = caps->flags;
        }
      if (caps->version > fallback_version)
        {
          fallback_version = caps->version;
          fallback_flags = caps->flags;
        }
    }

  uint32_t version;
  uint32_t flags;
  gboolean have_avc420;
  if (best_avc_version != 0)
    {
      version = best_avc_version;
      flags = best_avc_flags;
      have_avc420 = TRUE;
    }
  else
    {
      version = fallback_version;
      flags = fallback_flags;
      have_avc420 = FALSE;
      g_warning ("RDPGFX: no AVC-capable cap set advertised; falling back to "
                 "version 0x%08X without AVC", version);
    }

  /* Reject mid-session AVC downgrade. */
  if (pipeline->caps_received)
    {
      if (pipeline->have_avc420 && !have_avc420)
        {
          g_warning ("RDPGFX: ignoring cap re-advertise that would disable AVC420 "
                     "mid-session (version 0x%08X, flags 0x%08X)",
                     version, flags);
          g_mutex_unlock (&pipeline->mutex);
          /* Ack with previous capset so client doesn't hang. */
          RDPGFX_CAPSET keep = {0};
          keep.version = pipeline->negotiated_caps_version;
          keep.length = 4;
          keep.flags = pipeline->negotiated_caps_flags;
          caps_confirm.capsSet = &keep;
          return context->CapsConfirm (context, &caps_confirm);
        }
    }

  /* AVC444v2 disabled — aux view not implemented. */
  pipeline->have_avc444 = FALSE;
  pipeline->have_avc420 = have_avc420;
  pipeline->negotiated_caps_version = version;
  pipeline->negotiated_caps_flags = flags;

  g_message ("RDPGFX: Selected version 0x%08X flags 0x%08X, AVC444=%s, AVC420=%s",
             version, flags,
             pipeline->have_avc444 ? "yes" : "no",
             pipeline->have_avc420 ? "yes" : "no");

  RDPGFX_CAPSET capset = {0};
  capset.version = version;
  capset.length = 4;
  capset.flags = flags;
  caps_confirm.capsSet = &capset;

  pipeline->caps_received = TRUE;

  rdpSettings *settings = context->rdpcontext->settings;
  freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444v2, pipeline->have_avc444);
  freerdp_settings_set_bool (settings, FreeRDP_GfxAVC444, pipeline->have_avc444);
  freerdp_settings_set_bool (settings, FreeRDP_GfxH264, pipeline->have_avc420);

  g_mutex_unlock (&pipeline->mutex);

  /* CapsConfirm only — ResetGraphics in this callback corrupts FreeRDP
   * state (concurrent DVC read/write). Session thread sends it next tick. */
  return context->CapsConfirm (context, &caps_confirm);
}

/* Drops fast on bad window / sharp RTT rise; raises slow after stable. */
static void
adaptive_tick (MrdRdpGraphicsPipeline *pipeline,
               int64_t                 window_rtt_avg_us,
               uint64_t                window_occupancy_sum,
               uint32_t                window_occupancy_samples)
{
  g_mutex_lock (&pipeline->mutex);

  if (!pipeline->adaptive_enabled || window_occupancy_samples == 0)
    {
      /* Anchor still updated — re-enable wouldn't false-fire. */
      pipeline->prev_window_rtt_avg_us = window_rtt_avg_us;
      g_mutex_unlock (&pipeline->mutex);
      return;
    }

  double avg_occupancy = (double) window_occupancy_sum /
                         (double) window_occupancy_samples;
  /* Ratio = fraction of pipeline budget in use. */
  double occupancy_ratio = avg_occupancy /
                           (double) pipeline->adaptive_target_occupancy;

  int64_t rtt_prev = pipeline->prev_window_rtt_avg_us;
  gboolean rtt_sharp_rise = (rtt_prev > 0) &&
                            (window_rtt_avg_us > rtt_prev + rtt_prev / 2);
  gboolean rtt_stable     = (rtt_prev > 0) &&
                            (llabs (window_rtt_avg_us - rtt_prev) < rtt_prev / 10);

  int new_target = pipeline->adaptive_current_mbps;

  if (occupancy_ratio >= 0.9 || rtt_sharp_rise)
    {
      pipeline->high_occupancy_windows++;
      pipeline->stable_windows = 0;
      if (pipeline->high_occupancy_windows >= 2 || rtt_sharp_rise)
        {
          int drop = MAX (1, pipeline->adaptive_current_mbps * 12 / 100);
          new_target = MAX (pipeline->adaptive_min_mbps,
                            pipeline->adaptive_current_mbps - drop);
          pipeline->high_occupancy_windows = 0;
        }
    }
  else if (occupancy_ratio <= 0.5 && rtt_stable)
    {
      pipeline->stable_windows++;
      pipeline->high_occupancy_windows = 0;
      if (pipeline->stable_windows >= 3)
        {
          new_target = MIN (pipeline->adaptive_max_mbps,
                            pipeline->adaptive_current_mbps + 1);
          pipeline->stable_windows = 0;
        }
    }
  else
    {
      pipeline->high_occupancy_windows = 0;
      pipeline->stable_windows = 0;
    }

  pipeline->prev_window_rtt_avg_us = window_rtt_avg_us;

  if (new_target == pipeline->adaptive_current_mbps)
    {
      g_mutex_unlock (&pipeline->mutex);
      return;
    }

  int old_target = pipeline->adaptive_current_mbps;
  MrdRdpGraphicsPipelineBitrateCallback cb = pipeline->bitrate_callback;
  void *ud = pipeline->bitrate_user_data;

  g_mutex_unlock (&pipeline->mutex);

  g_message ("RDPGFX adaptive: %d -> %d Mbps "
             "(occupancy=%.2f, rtt=%.1f ms, prev=%.1f ms)",
             old_target, new_target, occupancy_ratio,
             window_rtt_avg_us / 1000.0, rtt_prev / 1000.0);

  gboolean ok = cb ? cb (pipeline, new_target, ud) : FALSE;

  g_mutex_lock (&pipeline->mutex);
  if (ok)
    {
      pipeline->adaptive_current_mbps = new_target;
    }
  else
    {
      g_warning ("RDPGFX adaptive: bitrate change rejected; disabling controller");
      pipeline->adaptive_enabled = FALSE;
    }
  g_mutex_unlock (&pipeline->mutex);
}

static UINT
rdpgfx_frame_acknowledge (RdpgfxServerContext                 *context,
                          const RDPGFX_FRAME_ACKNOWLEDGE_PDU  *frame_ack)
{
  MrdRdpGraphicsPipeline *pipeline = context->custom;
  gpointer submit_val = NULL;
  gboolean found;
  int64_t rtt_us = -1;
  gboolean log_summary = FALSE;
  int64_t window_avg_us = 0, window_min_us = 0, window_max_us = 0;
  int64_t window_encode_avg_us = 0, window_encode_max_us = 0;
  int64_t window_send_avg_us = 0, window_send_max_us = 0;
  uint64_t window_bytes_avg = 0, window_bytes_max = 0;
  uint32_t window_samples = 0;
  uint64_t window_occupancy_sum = 0;
  uint32_t window_occupancy_samples = 0;

  g_mutex_lock (&pipeline->mutex);

  found = g_hash_table_steal_extended (pipeline->pending_frames,
                                       GUINT_TO_POINTER (frame_ack->frameId),
                                       NULL,
                                       &submit_val);
  if (found)
    {
      int64_t submit_us = (int64_t) (gssize) submit_val;
      int64_t now_us = g_get_monotonic_time ();

      rtt_us = now_us - submit_us;

      if (pipeline->rtt_samples == 0 || rtt_us < pipeline->rtt_min_us)
        pipeline->rtt_min_us = rtt_us;
      if (rtt_us > pipeline->rtt_max_us)
        pipeline->rtt_max_us = rtt_us;
      pipeline->rtt_sum_us += rtt_us;
      pipeline->rtt_samples++;

      if (pipeline->rtt_samples >= MRD_GFX_RTT_LOG_WINDOW)
        {
          log_summary = TRUE;
          window_samples = pipeline->rtt_samples;
          window_avg_us = pipeline->rtt_sum_us / (int64_t) pipeline->rtt_samples;
          window_min_us = pipeline->rtt_min_us;
          window_max_us = pipeline->rtt_max_us;

          window_encode_avg_us = pipeline->encode_sum_us / (int64_t) pipeline->rtt_samples;
          window_encode_max_us = pipeline->encode_max_us;
          window_send_avg_us   = pipeline->send_sum_us   / (int64_t) pipeline->rtt_samples;
          window_send_max_us   = pipeline->send_max_us;
          window_bytes_avg     = pipeline->bytes_sum / pipeline->rtt_samples;
          window_bytes_max     = pipeline->bytes_max;

          window_occupancy_sum     = pipeline->occupancy_sum;
          window_occupancy_samples = pipeline->occupancy_samples;

          pipeline->rtt_samples = 0;
          pipeline->rtt_sum_us = 0;
          pipeline->rtt_min_us = 0;
          pipeline->rtt_max_us = 0;
          pipeline->encode_sum_us = 0;
          pipeline->encode_max_us = 0;
          pipeline->send_sum_us = 0;
          pipeline->send_max_us = 0;
          pipeline->bytes_sum = 0;
          pipeline->bytes_max = 0;
          pipeline->occupancy_sum = 0;
          pipeline->occupancy_samples = 0;
        }

      if (pipeline->frame_ack_callback)
        {
          pipeline->frame_ack_callback (pipeline,
                                        frame_ack->frameId,
                                        pipeline->frame_ack_user_data);
        }
    }

  if (frame_ack->queueDepth == MRD_GFX_SUSPEND_QUEUE_DEPTH)
    {
      if (!pipeline->acks_suspended)
        g_message ("RDPGFX: client requested ACK suspend "
                   "(frameId=%u, totalDecoded=%u)",
                   frame_ack->frameId, frame_ack->totalFramesDecoded);
      pipeline->acks_suspended = TRUE;

      /* Drain pending → fire frame_ack_callback so frames_in_flight returns to 0. */
      GHashTableIter iter;
      gpointer key;
      g_hash_table_iter_init (&iter, pipeline->pending_frames);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        {
          uint32_t drained_fid = GPOINTER_TO_UINT (key);
          if (pipeline->frame_ack_callback)
            pipeline->frame_ack_callback (pipeline, drained_fid,
                                          pipeline->frame_ack_user_data);
        }
      g_hash_table_remove_all (pipeline->pending_frames);

      /* Drop in-progress window so partial samples don't skew next summary. */
      pipeline->rtt_samples = 0;
      pipeline->rtt_sum_us = 0;
      pipeline->rtt_min_us = 0;
      pipeline->rtt_max_us = 0;
      pipeline->encode_sum_us = 0;
      pipeline->encode_max_us = 0;
      pipeline->send_sum_us = 0;
      pipeline->send_max_us = 0;
      pipeline->bytes_sum = 0;
      pipeline->bytes_max = 0;
      log_summary = FALSE;
    }
  else if (pipeline->acks_suspended)
    {
      g_message ("RDPGFX: client resumed ACK delivery "
                 "(frameId=%u, queueDepth=%u)",
                 frame_ack->frameId, frame_ack->queueDepth);
      pipeline->acks_suspended = FALSE;
    }
  pipeline->last_queue_depth = frame_ack->queueDepth;
  pipeline->last_total_decoded = frame_ack->totalFramesDecoded;

  g_mutex_unlock (&pipeline->mutex);

  if (rtt_us >= 0)
    g_debug ("RDPGFX: Frame %u acked, submit→ack %.2f ms",
             frame_ack->frameId, rtt_us / 1000.0);
  else
    g_debug ("RDPGFX: Frame %u acked (no submit timestamp)",
             frame_ack->frameId);

  if (log_summary)
    {
      /* wire+client ≈ rtt - send (rtt fully contains the synchronous send). */
      int64_t window_wire_avg_us = window_avg_us - window_send_avg_us;
      int64_t window_wire_max_us = window_max_us - window_send_max_us;
      if (window_wire_avg_us < 0) window_wire_avg_us = 0;
      if (window_wire_max_us < 0) window_wire_max_us = 0;

      MRD_TELEMETRY_LOG ("RDPGFX stats (last %u frames):"
                         " rtt avg=%.1f/min=%.1f/max=%.1f ms |"
                         " encode avg=%.1f/max=%.1f ms |"
                         " send avg=%.1f/max=%.1f ms |"
                         " wire~avg=%.1f/max=%.1f ms |"
                         " bytes avg=%.1f/max=%.1f KB",
                         window_samples,
                         window_avg_us / 1000.0,
                         window_min_us / 1000.0,
                         window_max_us / 1000.0,
                         window_encode_avg_us / 1000.0,
                         window_encode_max_us / 1000.0,
                         window_send_avg_us / 1000.0,
                         window_send_max_us / 1000.0,
                         window_wire_avg_us / 1000.0,
                         window_wire_max_us / 1000.0,
                         window_bytes_avg / 1024.0,
                         window_bytes_max / 1024.0);

      adaptive_tick (pipeline, window_avg_us,
                     window_occupancy_sum, window_occupancy_samples);
    }

  return CHANNEL_RC_OK;
}

static UINT
rdpgfx_qoe_frame_acknowledge (RdpgfxServerContext                     *context,
                              const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU  *qoe_ack)
{
  g_debug ("RDPGFX: QoE frame %u acknowledged (timestamp %u)",
           qoe_ack->frameId, qoe_ack->timestamp);
  return CHANNEL_RC_OK;
}

static UINT
rdpgfx_cache_import_offer (RdpgfxServerContext                *context,
                           const RDPGFX_CACHE_IMPORT_OFFER_PDU *cache_offer)
{
  RDPGFX_CACHE_IMPORT_REPLY_PDU reply = {0};

  g_debug ("RDPGFX: Cache import offer (ignored)");

  return context->CacheImportReply (context, &reply);
}

static BOOL
rdpgfx_channel_id_assigned (RdpgfxServerContext *context, UINT32 channel_id)
{
  g_debug ("RDPGFX: Channel ID %u assigned", channel_id);
  return TRUE;
}

/* Pointers alias pipeline buffers; caller must not free. SurfaceCommand
 * serializes synchronously, so per-frame reuse is safe. */
static void
prepare_avc420_meta (MrdRdpGraphicsPipeline      *pipeline,
                     RDPGFX_AVC420_BITMAP_STREAM *avc420,
                     MrdBitstream                *bitstream,
                     cairo_region_t              *damage_region)
{
  guint n_rects = (guint) cairo_region_num_rectangles (damage_region);

  avc420->data = mrd_bitstream_get_data (bitstream);
  avc420->length = mrd_bitstream_get_length (bitstream);

  /* Double-on-growth, start at 16 (covers typical SCK dirty-rect count). */
  if (n_rects > pipeline->meta_rects_cap)
    {
      guint new_cap = pipeline->meta_rects_cap ? pipeline->meta_rects_cap * 2 : 16;
      if (new_cap < n_rects)
        new_cap = n_rects;
      pipeline->meta_rects_buf = g_renew (RECTANGLE_16,
                                          pipeline->meta_rects_buf, new_cap);
      pipeline->meta_qq_buf    = g_renew (RDPGFX_H264_QUANT_QUALITY,
                                          pipeline->meta_qq_buf, new_cap);
      pipeline->meta_rects_cap = new_cap;
    }

  avc420->meta.numRegionRects    = n_rects;
  avc420->meta.regionRects       = pipeline->meta_rects_buf;
  avc420->meta.quantQualityVals  = pipeline->meta_qq_buf;

  for (guint i = 0; i < n_rects; i++)
    {
      cairo_rectangle_int_t rect;
      cairo_region_get_rectangle (damage_region, (int) i, &rect);

      pipeline->meta_rects_buf[i].left   = rect.x;
      pipeline->meta_rects_buf[i].top    = rect.y;
      pipeline->meta_rects_buf[i].right  = rect.x + rect.width;
      pipeline->meta_rects_buf[i].bottom = rect.y + rect.height;

      pipeline->meta_qq_buf[i].qp         = 22;
      pipeline->meta_qq_buf[i].qualityVal = 100;
      pipeline->meta_qq_buf[i].r          = 0;
      pipeline->meta_qq_buf[i].p          = 0;
    }
}

/* Drop aliased pointers so nothing downstream holds pipeline storage. */
static void
clear_avc420_meta (RDPGFX_AVC420_BITMAP_STREAM *avc420)
{
  avc420->meta.numRegionRects = 0;
  avc420->meta.regionRects = NULL;
  avc420->meta.quantQualityVals = NULL;
}

/* MS-RDPEGFX 2.2.4.5: numRects(4) + rects(8/each) + qq(2/each) + data. */
static uint32_t
calculate_avc420_size (RDPGFX_AVC420_BITMAP_STREAM *avc420)
{
  uint32_t size = 0;
  size += 4;
  size += avc420->meta.numRegionRects * 8;
  size += avc420->meta.numRegionRects * 2;
  size += avc420->length;
  return size;
}

MrdRdpGraphicsPipeline *
mrd_rdp_graphics_pipeline_new (MrdSessionRdp *session_rdp,
                               HANDLE         vcm,
                               rdpContext    *rdp_context)
{
  MrdRdpGraphicsPipeline *pipeline;
  RdpgfxServerContext *context;

  pipeline = g_object_new (MRD_TYPE_RDP_GRAPHICS_PIPELINE, NULL);
  pipeline->session_rdp = session_rdp;

  context = rdpgfx_server_context_new (vcm);
  if (!context)
    {
      g_warning ("Failed to create RDPGFX server context");
      g_object_unref (pipeline);
      return NULL;
    }

  pipeline->rdpgfx_context = context;
  context->custom = pipeline;
  context->rdpcontext = rdp_context;

  /* Don't override Open/Close — those are FreeRDP methods. */
  context->CapsAdvertise = rdpgfx_caps_advertise;
  context->FrameAcknowledge = rdpgfx_frame_acknowledge;
  context->QoeFrameAcknowledge = rdpgfx_qoe_frame_acknowledge;
  context->CacheImportOffer = rdpgfx_cache_import_offer;
  context->ChannelIdAssigned = rdpgfx_channel_id_assigned;

  pipeline->rfx_context = rfx_context_new (TRUE);
  pipeline->encode_stream = Stream_New (NULL, 64 * 64 * 4);

  return pipeline;
}

gboolean
mrd_rdp_graphics_pipeline_open_channel (MrdRdpGraphicsPipeline *pipeline)
{
  RdpgfxServerContext *rdpgfx_context;

  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  rdpgfx_context = pipeline->rdpgfx_context;

  /* External-thread mode: session thread pumps via handle_messages. */
  if (!rdpgfx_context->Initialize (rdpgfx_context, TRUE))
    {
      g_warning ("RDPGFX: Initialize failed");
      return FALSE;
    }

  if (!rdpgfx_context->Open (rdpgfx_context))
    {
      g_warning ("RDPGFX: Open failed");
      return FALSE;
    }

  pipeline->channel_opened = TRUE;
  g_message ("RDPGFX channel opened (external thread mode)");
  return TRUE;
}

void
mrd_rdp_graphics_pipeline_stop (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  if (pipeline->rdpgfx_context && pipeline->channel_opened)
    {
      pipeline->rdpgfx_context->Close (pipeline->rdpgfx_context);
      pipeline->channel_opened = FALSE;
      g_message ("RDPGFX pipeline stopped");
    }

  pipeline->initialized = FALSE;
  pipeline->caps_received = FALSE;
  pipeline->reset_sent = FALSE;
}

HANDLE
mrd_rdp_graphics_pipeline_get_event_handle (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), NULL);

  return rdpgfx_server_get_event_handle (pipeline->rdpgfx_context);
}

gboolean
mrd_rdp_graphics_pipeline_handle_messages (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  g_debug ("RDPGFX: Handling messages...");

  UINT status = rdpgfx_server_handle_messages (pipeline->rdpgfx_context);
  if (status != CHANNEL_RC_OK)
    {
      g_warning ("RDPGFX handle_messages failed: %u", status);
      return FALSE;
    }

  return TRUE;
}

gboolean
mrd_rdp_graphics_pipeline_is_ready (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  g_mutex_lock (&pipeline->mutex);
  gboolean ready = pipeline->initialized;
  g_mutex_unlock (&pipeline->mutex);

  return ready;
}

gboolean
mrd_rdp_graphics_pipeline_needs_reset (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  g_mutex_lock (&pipeline->mutex);
  gboolean needs = pipeline->caps_received && !pipeline->reset_sent &&
                   (pipeline->have_avc444 || pipeline->have_avc420);
  g_mutex_unlock (&pipeline->mutex);

  return needs;
}

gboolean
mrd_rdp_graphics_pipeline_send_reset_graphics (MrdRdpGraphicsPipeline *pipeline,
                                                uint32_t                width,
                                                uint32_t                height)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  RdpgfxServerContext *context = pipeline->rdpgfx_context;

  MONITOR_DEF monitor = {0};
  monitor.left = 0;
  monitor.top = 0;
  monitor.right = (INT32)width - 1;
  monitor.bottom = (INT32)height - 1;
  monitor.flags = MONITOR_PRIMARY;

  RDPGFX_RESET_GRAPHICS_PDU reset = {0};
  reset.width = width;
  reset.height = height;
  reset.monitorCount = 1;
  reset.monitorDefArray = &monitor;

  UINT rc = context->ResetGraphics (context, &reset);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("RDPGFX: ResetGraphics failed: %u", rc);
      return FALSE;
    }

  g_mutex_lock (&pipeline->mutex);
  pipeline->reset_sent = TRUE;
  pipeline->initialized = TRUE;
  g_mutex_unlock (&pipeline->mutex);

  g_message ("RDPGFX: ResetGraphics sent (%ux%u)", width, height);

  return TRUE;
}

void
mrd_rdp_graphics_pipeline_get_capabilities (MrdRdpGraphicsPipeline *pipeline,
                                            gboolean               *have_avc444,
                                            gboolean               *have_avc420)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);

  if (have_avc444)
    *have_avc444 = pipeline->have_avc444;
  if (have_avc420)
    *have_avc420 = pipeline->have_avc420;

  g_mutex_unlock (&pipeline->mutex);
}

gboolean
mrd_rdp_graphics_pipeline_create_surface (MrdRdpGraphicsPipeline *pipeline,
                                          uint16_t                surface_id,
                                          uint32_t                width,
                                          uint32_t                height,
                                          GError                **error)
{
  RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
  RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map_surface = {0};
  SurfaceInfo *info;
  UINT status;

  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  g_mutex_lock (&pipeline->mutex);

  create_surface.surfaceId = surface_id;
  create_surface.width = width;
  create_surface.height = height;
  create_surface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

  status = pipeline->rdpgfx_context->CreateSurface (pipeline->rdpgfx_context,
                                                    &create_surface);
  if (status != CHANNEL_RC_OK)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create RDPGFX surface: %u", status);
      return FALSE;
    }

  map_surface.surfaceId = surface_id;
  map_surface.outputOriginX = 0;
  map_surface.outputOriginY = 0;
  map_surface.reserved = 0;

  status = pipeline->rdpgfx_context->MapSurfaceToOutput (pipeline->rdpgfx_context,
                                                         &map_surface);
  if (status != CHANNEL_RC_OK)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to map surface to output: %u", status);
      return FALSE;
    }

  info = g_new0 (SurfaceInfo, 1);
  info->surface_id = surface_id;
  info->width = width;
  info->height = height;
  info->mapped_to_output = TRUE;

  g_hash_table_insert (pipeline->surfaces,
                       GUINT_TO_POINTER (surface_id),
                       info);

  g_mutex_unlock (&pipeline->mutex);

  g_message ("Created RDPGFX surface %u (%ux%u)", surface_id, width, height);

  return TRUE;
}

void
mrd_rdp_graphics_pipeline_delete_surface (MrdRdpGraphicsPipeline *pipeline,
                                          uint16_t                surface_id)
{
  RDPGFX_DELETE_SURFACE_PDU delete_surface = {0};

  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);

  delete_surface.surfaceId = surface_id;
  pipeline->rdpgfx_context->DeleteSurface (pipeline->rdpgfx_context,
                                           &delete_surface);

  g_hash_table_remove (pipeline->surfaces, GUINT_TO_POINTER (surface_id));

  g_mutex_unlock (&pipeline->mutex);

  g_message ("Deleted RDPGFX surface %u", surface_id);
}

gboolean
mrd_rdp_graphics_pipeline_submit_frame (MrdRdpGraphicsPipeline *pipeline,
                                        uint16_t                surface_id,
                                        MrdBitstream           *main_bitstream,
                                        MrdBitstream           *aux_bitstream,
                                        cairo_region_t         *damage_region,
                                        int64_t                 encode_us,
                                        size_t                  payload_bytes,
                                        GError                **error)
{
  RDPGFX_START_FRAME_PDU start_frame = {0};
  RDPGFX_END_FRAME_PDU end_frame = {0};
  RDPGFX_SURFACE_COMMAND cmd = {0};
  UINT status;
  uint32_t frame_id;
  int64_t submit_us;

  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);
  g_return_val_if_fail (main_bitstream != NULL, FALSE);

  /* Channel writes run unlocked (session thread is the only writer); the
   * mutex guards shared state touched by the ACK-thread callback. */
  g_mutex_lock (&pipeline->mutex);
  frame_id = pipeline->next_frame_id++;
  SurfaceInfo *surface_info = g_hash_table_lookup (pipeline->surfaces,
                                                    GUINT_TO_POINTER (surface_id));
  if (!surface_info)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Surface %u not found", surface_id);
      return FALSE;
    }
  uint32_t surf_w = surface_info->width;
  uint32_t surf_h = surface_info->height;
  gboolean have_avc444 = pipeline->have_avc444;
  gboolean have_avc420 = pipeline->have_avc420;
  g_mutex_unlock (&pipeline->mutex);

  submit_us = g_get_monotonic_time ();

  start_frame.frameId = frame_id;
  start_frame.timestamp = submit_us / 1000;

  status = pipeline->rdpgfx_context->StartFrame (pipeline->rdpgfx_context,
                                                 &start_frame);
  if (status != CHANNEL_RC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to start frame: %u", status);
      return FALSE;
    }

  cmd.surfaceId = surface_id;
  cmd.format = PIXEL_FORMAT_BGRX32;

  /* Full surface — partial-damage hints ride on meta.regionRects.
   * Shrinking to damage extents leaves stale pixels under disjoint damage. */
  cmd.left   = 0;
  cmd.top    = 0;
  cmd.right  = surf_w;
  cmd.bottom = surf_h;
  cmd.width  = surf_w;
  cmd.height = surf_h;

  if (have_avc444 && aux_bitstream)
    {
      RDPGFX_AVC444_BITMAP_STREAM avc444 = {0};

      cmd.codecId = RDPGFX_CODECID_AVC444v2;
      avc444.LC = 0;   /* DUAL mode */

      /* Both views share meta_* (same content, SurfaceCommand serial). */
      prepare_avc420_meta (pipeline, &avc444.bitstream[0], main_bitstream, damage_region);
      prepare_avc420_meta (pipeline, &avc444.bitstream[1], aux_bitstream, damage_region);

      avc444.cbAvc420EncodedBitstream1 = calculate_avc420_size (&avc444.bitstream[0]);

      cmd.extra = &avc444;

      status = pipeline->rdpgfx_context->SurfaceCommand (pipeline->rdpgfx_context,
                                                         &cmd);

      clear_avc420_meta (&avc444.bitstream[0]);
      clear_avc420_meta (&avc444.bitstream[1]);
    }
  else if (have_avc420)
    {
      RDPGFX_AVC420_BITMAP_STREAM avc420 = {0};

      cmd.codecId = RDPGFX_CODECID_AVC420;
      prepare_avc420_meta (pipeline, &avc420, main_bitstream, damage_region);
      cmd.extra = &avc420;

      status = pipeline->rdpgfx_context->SurfaceCommand (pipeline->rdpgfx_context,
                                                         &cmd);

      clear_avc420_meta (&avc420);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Client does not support AVC encoding");
      return FALSE;
    }

  if (status != CHANNEL_RC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to send surface command: %u", status);
      return FALSE;
    }

  end_frame.frameId = frame_id;

  status = pipeline->rdpgfx_context->EndFrame (pipeline->rdpgfx_context,
                                               &end_frame);
  if (status != CHANNEL_RC_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end frame: %u", status);
      return FALSE;
    }

  int64_t send_us = g_get_monotonic_time () - submit_us;

  g_mutex_lock (&pipeline->mutex);

  g_hash_table_insert (pipeline->pending_frames,
                       GUINT_TO_POINTER (frame_id),
                       (gpointer) (gssize) submit_us);

  pipeline->send_sum_us += send_us;
  if (send_us > pipeline->send_max_us)
    pipeline->send_max_us = send_us;

  if (encode_us > 0)
    {
      pipeline->encode_sum_us += encode_us;
      if (encode_us > pipeline->encode_max_us)
        pipeline->encode_max_us = encode_us;
    }

  pipeline->bytes_sum += payload_bytes;
  if ((uint64_t) payload_bytes > pipeline->bytes_max)
    pipeline->bytes_max = payload_bytes;

  g_mutex_unlock (&pipeline->mutex);

  g_debug ("Submitted frame %u (surface %u) encode=%.1fms send=%.1fms bytes=%zu",
           frame_id, surface_id,
           encode_us / 1000.0, send_us / 1000.0, payload_bytes);

  return TRUE;
}

void
mrd_rdp_graphics_pipeline_set_frame_ack_callback (MrdRdpGraphicsPipeline                 *pipeline,
                                                  MrdRdpGraphicsPipelineFrameAckCallback  callback,
                                                  void                                   *user_data)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);
  pipeline->frame_ack_callback = callback;
  pipeline->frame_ack_user_data = user_data;
  g_mutex_unlock (&pipeline->mutex);
}

void
mrd_rdp_graphics_pipeline_set_bitrate_callback (MrdRdpGraphicsPipeline                *pipeline,
                                                MrdRdpGraphicsPipelineBitrateCallback  callback,
                                                void                                  *user_data)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);
  pipeline->bitrate_callback = callback;
  pipeline->bitrate_user_data = user_data;
  g_mutex_unlock (&pipeline->mutex);
}

void
mrd_rdp_graphics_pipeline_configure_adaptive (MrdRdpGraphicsPipeline *pipeline,
                                              gboolean                enabled,
                                              int                     initial_mbps,
                                              int                     min_mbps,
                                              int                     max_mbps,
                                              guint                   target_occupancy)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);
  pipeline->adaptive_enabled = enabled;
  pipeline->adaptive_min_mbps = min_mbps;
  pipeline->adaptive_max_mbps = max_mbps;
  pipeline->adaptive_current_mbps = initial_mbps;
  pipeline->adaptive_target_occupancy = target_occupancy > 0 ? target_occupancy : 1;
  pipeline->high_occupancy_windows = 0;
  pipeline->stable_windows = 0;
  pipeline->prev_window_rtt_avg_us = 0;
  pipeline->occupancy_sum = 0;
  pipeline->occupancy_samples = 0;
  g_mutex_unlock (&pipeline->mutex);

  if (enabled)
    g_message ("RDPGFX adaptive controller: enabled, start=%d Mbps, bounds=[%d, %d], target_occupancy=%u",
               initial_mbps, min_mbps, max_mbps, target_occupancy);
  else
    g_message ("RDPGFX adaptive controller: disabled (MRD_ADAPTIVE=0 or static config)");
}

void
mrd_rdp_graphics_pipeline_record_occupancy (MrdRdpGraphicsPipeline *pipeline,
                                            guint                   occupancy)
{
  g_return_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline));

  g_mutex_lock (&pipeline->mutex);
  if (pipeline->adaptive_enabled)
    {
      pipeline->occupancy_sum += occupancy;
      pipeline->occupancy_samples++;
    }
  g_mutex_unlock (&pipeline->mutex);
}

gboolean
mrd_rdp_graphics_pipeline_acks_suspended (MrdRdpGraphicsPipeline *pipeline)
{
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);

  g_mutex_lock (&pipeline->mutex);
  gboolean suspended = pipeline->acks_suspended;
  g_mutex_unlock (&pipeline->mutex);
  return suspended;
}

/* GObject boilerplate */

static void
mrd_rdp_graphics_pipeline_finalize (GObject *object)
{
  MrdRdpGraphicsPipeline *pipeline = MRD_RDP_GRAPHICS_PIPELINE (object);

  mrd_rdp_graphics_pipeline_stop (pipeline);

  g_clear_pointer (&pipeline->surfaces, g_hash_table_unref);
  g_clear_pointer (&pipeline->pending_frames, g_hash_table_unref);
  g_clear_pointer (&pipeline->meta_rects_buf, g_free);
  g_clear_pointer (&pipeline->meta_qq_buf, g_free);

  if (pipeline->encode_stream)
    Stream_Free (pipeline->encode_stream, TRUE);

  if (pipeline->rfx_context)
    rfx_context_free (pipeline->rfx_context);

  if (pipeline->rdpgfx_context)
    rdpgfx_server_context_free (pipeline->rdpgfx_context);

  g_mutex_clear (&pipeline->mutex);

  G_OBJECT_CLASS (mrd_rdp_graphics_pipeline_parent_class)->finalize (object);
}

static void
mrd_rdp_graphics_pipeline_init (MrdRdpGraphicsPipeline *pipeline)
{
  g_mutex_init (&pipeline->mutex);

  pipeline->surfaces = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, g_free);
  pipeline->pending_frames = g_hash_table_new (g_direct_hash, g_direct_equal);

  pipeline->next_frame_id = 1;
}

static void
mrd_rdp_graphics_pipeline_class_init (MrdRdpGraphicsPipelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mrd_rdp_graphics_pipeline_finalize;
}
