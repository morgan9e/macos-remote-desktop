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
  
  RFX_CONTEXT *rfx_context;
  wStream *encode_stream;
  
  GHashTable *surfaces;  
  
  uint32_t next_frame_id;
  GHashTable *pending_frames;  
  
  MrdRdpGraphicsPipelineFrameAckCallback frame_ack_callback;
  void *frame_ack_user_data;
  GMutex mutex;
};
G_DEFINE_TYPE (MrdRdpGraphicsPipeline, mrd_rdp_graphics_pipeline, G_TYPE_OBJECT)

typedef struct {
  uint16_t surface_id;
  uint32_t width;
  uint32_t height;
  gboolean mapped_to_output;
} SurfaceInfo;

static UINT
rdpgfx_caps_advertise (RdpgfxServerContext             *context,
                       const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise)
{
  MrdRdpGraphicsPipeline *pipeline = context->custom;
  RDPGFX_CAPS_CONFIRM_PDU caps_confirm = {0};
  uint32_t version = 0;
  uint32_t flags = 0;
  g_mutex_lock (&pipeline->mutex);
  g_debug ("RDPGFX: Received caps advertise with %u cap sets",
           caps_advertise->capsSetCount);
  
  for (uint16_t i = 0; i < caps_advertise->capsSetCount; i++)
    {
      const RDPGFX_CAPSET *caps = &caps_advertise->capsSets[i];
      g_debug ("  Cap set %u: version 0x%08X, flags 0x%08X",
               i, caps->version, caps->flags);
      
      if (caps->version > version)
        {
          version = caps->version;
          flags = caps->flags;
        }
    }
  
  if (version >= RDPGFX_CAPVERSION_102)
    {
      pipeline->have_avc444 = FALSE;
      pipeline->have_avc420 = TRUE;
    }
  g_message ("RDPGFX: Selected version 0x%08X, AVC444=%s, AVC420=%s",
             version,
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
  
  return context->CapsConfirm (context, &caps_confirm);
}
static UINT
rdpgfx_frame_acknowledge (RdpgfxServerContext                 *context,
                          const RDPGFX_FRAME_ACKNOWLEDGE_PDU  *frame_ack)
{
  MrdRdpGraphicsPipeline *pipeline = context->custom;
  g_debug ("RDPGFX: Frame %u acknowledged", frame_ack->frameId);
  g_mutex_lock (&pipeline->mutex);
  if (g_hash_table_remove (pipeline->pending_frames,
                           GUINT_TO_POINTER (frame_ack->frameId)))
    {
      if (pipeline->frame_ack_callback)
        {
          pipeline->frame_ack_callback (pipeline,
                                        frame_ack->frameId,
                                        pipeline->frame_ack_user_data);
        }
    }
  g_mutex_unlock (&pipeline->mutex);
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

static void
prepare_avc420_meta (RDPGFX_AVC420_BITMAP_STREAM *avc420,
                     MrdBitstream                *bitstream,
                     cairo_region_t              *damage_region)
{
  int n_rects = cairo_region_num_rectangles (damage_region);
  avc420->data = mrd_bitstream_get_data (bitstream);
  avc420->length = mrd_bitstream_get_length (bitstream);
  
  avc420->meta.numRegionRects = n_rects;
  avc420->meta.regionRects = g_new0 (RECTANGLE_16, n_rects);
  avc420->meta.quantQualityVals = g_new0 (RDPGFX_H264_QUANT_QUALITY, n_rects);
  for (int i = 0; i < n_rects; i++)
    {
      cairo_rectangle_int_t rect;
      cairo_region_get_rectangle (damage_region, i, &rect);
      avc420->meta.regionRects[i].left = rect.x;
      avc420->meta.regionRects[i].top = rect.y;
      avc420->meta.regionRects[i].right = rect.x + rect.width;
      avc420->meta.regionRects[i].bottom = rect.y + rect.height;
      
      avc420->meta.quantQualityVals[i].qp = 22;  
      avc420->meta.quantQualityVals[i].qualityVal = 100;
      avc420->meta.quantQualityVals[i].r = 0;
      avc420->meta.quantQualityVals[i].p = 0;
    }
}
static void
free_avc420_meta (RDPGFX_AVC420_BITMAP_STREAM *avc420)
{
  g_free (avc420->meta.regionRects);
  g_free (avc420->meta.quantQualityVals);
}

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
                                        GError                **error)
{
  RDPGFX_START_FRAME_PDU start_frame = {0};
  RDPGFX_END_FRAME_PDU end_frame = {0};
  RDPGFX_SURFACE_COMMAND cmd = {0};
  UINT status;
  uint32_t frame_id;
  g_return_val_if_fail (MRD_IS_RDP_GRAPHICS_PIPELINE (pipeline), FALSE);
  g_return_val_if_fail (main_bitstream != NULL, FALSE);
  g_mutex_lock (&pipeline->mutex);
  frame_id = pipeline->next_frame_id++;
  
  start_frame.frameId = frame_id;
  start_frame.timestamp = g_get_monotonic_time () / 1000;
  status = pipeline->rdpgfx_context->StartFrame (pipeline->rdpgfx_context,
                                                 &start_frame);
  if (status != CHANNEL_RC_OK)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to start frame: %u", status);
      return FALSE;
    }
  
  SurfaceInfo *surface_info = g_hash_table_lookup (pipeline->surfaces,
                                                    GUINT_TO_POINTER (surface_id));
  if (!surface_info)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Surface %u not found", surface_id);
      return FALSE;
    }
  
  cmd.surfaceId = surface_id;
  cmd.format = PIXEL_FORMAT_BGRX32;
  
  cmd.left = 0;
  cmd.top = 0;
  cmd.right = surface_info->width;
  cmd.bottom = surface_info->height;
  cmd.width = surface_info->width;
  cmd.height = surface_info->height;
  if (pipeline->have_avc444 && aux_bitstream)
    {
      
      RDPGFX_AVC444_BITMAP_STREAM avc444 = {0};
      cmd.codecId = RDPGFX_CODECID_AVC444v2;
      
      avc444.LC = 0;
      prepare_avc420_meta (&avc444.bitstream[0], main_bitstream, damage_region);
      prepare_avc420_meta (&avc444.bitstream[1], aux_bitstream, damage_region);
      
      avc444.cbAvc420EncodedBitstream1 = calculate_avc420_size (&avc444.bitstream[0]);
      cmd.extra = &avc444;
      status = pipeline->rdpgfx_context->SurfaceCommand (pipeline->rdpgfx_context,
                                                         &cmd);
      free_avc420_meta (&avc444.bitstream[0]);
      free_avc420_meta (&avc444.bitstream[1]);
    }
  else if (pipeline->have_avc420)
    {
      
      RDPGFX_AVC420_BITMAP_STREAM avc420 = {0};
      cmd.codecId = RDPGFX_CODECID_AVC420;
      prepare_avc420_meta (&avc420, main_bitstream, damage_region);
      cmd.extra = &avc420;
      status = pipeline->rdpgfx_context->SurfaceCommand (pipeline->rdpgfx_context,
                                                         &cmd);
      free_avc420_meta (&avc420);
    }
  else
    {
      
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Client does not support AVC encoding");
      return FALSE;
    }
  if (status != CHANNEL_RC_OK)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to send surface command: %u", status);
      return FALSE;
    }
  
  end_frame.frameId = frame_id;
  status = pipeline->rdpgfx_context->EndFrame (pipeline->rdpgfx_context,
                                               &end_frame);
  if (status != CHANNEL_RC_OK)
    {
      g_mutex_unlock (&pipeline->mutex);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to end frame: %u", status);
      return FALSE;
    }
  
  g_hash_table_insert (pipeline->pending_frames,
                       GUINT_TO_POINTER (frame_id),
                       GINT_TO_POINTER (start_frame.timestamp));
  g_mutex_unlock (&pipeline->mutex);
  g_debug ("Submitted frame %u (surface %u)", frame_id, surface_id);
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

static void
mrd_rdp_graphics_pipeline_finalize (GObject *object)
{
  MrdRdpGraphicsPipeline *pipeline = MRD_RDP_GRAPHICS_PIPELINE (object);
  mrd_rdp_graphics_pipeline_stop (pipeline);
  g_clear_pointer (&pipeline->surfaces, g_hash_table_unref);
  g_clear_pointer (&pipeline->pending_frames, g_hash_table_unref);
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