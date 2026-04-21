#pragma once

#include <glib-object.h>
#include <cairo.h>
#include <freerdp/server/rdpgfx.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_RDP_GRAPHICS_PIPELINE (mrd_rdp_graphics_pipeline_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpGraphicsPipeline, mrd_rdp_graphics_pipeline,
                      MRD, RDP_GRAPHICS_PIPELINE, GObject)

MrdRdpGraphicsPipeline *mrd_rdp_graphics_pipeline_new (MrdSessionRdp *session_rdp,
                                                        HANDLE         vcm,
                                                        rdpContext    *rdp_context);

gboolean mrd_rdp_graphics_pipeline_open_channel (MrdRdpGraphicsPipeline *pipeline);

void mrd_rdp_graphics_pipeline_stop (MrdRdpGraphicsPipeline *pipeline);

HANDLE mrd_rdp_graphics_pipeline_get_event_handle (MrdRdpGraphicsPipeline *pipeline);
gboolean mrd_rdp_graphics_pipeline_handle_messages (MrdRdpGraphicsPipeline *pipeline);

gboolean mrd_rdp_graphics_pipeline_is_ready (MrdRdpGraphicsPipeline *pipeline);
gboolean mrd_rdp_graphics_pipeline_needs_reset (MrdRdpGraphicsPipeline *pipeline);

/* Must be called OUTSIDE handle_messages(). */
gboolean mrd_rdp_graphics_pipeline_send_reset_graphics (MrdRdpGraphicsPipeline *pipeline,
                                                         uint32_t                width,
                                                         uint32_t                height);

void mrd_rdp_graphics_pipeline_get_capabilities (MrdRdpGraphicsPipeline *pipeline,
                                                 gboolean               *have_avc444,
                                                 gboolean               *have_avc420);

gboolean mrd_rdp_graphics_pipeline_create_surface (MrdRdpGraphicsPipeline *pipeline,
                                                   uint16_t                surface_id,
                                                   uint32_t                width,
                                                   uint32_t                height,
                                                   GError                **error);

void mrd_rdp_graphics_pipeline_delete_surface (MrdRdpGraphicsPipeline *pipeline,
                                               uint16_t                surface_id);

/* encode_us / payload_bytes are diagnostic; pass 0 if unavailable. */
gboolean mrd_rdp_graphics_pipeline_submit_frame (MrdRdpGraphicsPipeline *pipeline,
                                                 uint16_t                surface_id,
                                                 MrdBitstream           *main_bitstream,
                                                 MrdBitstream           *aux_bitstream,
                                                 cairo_region_t         *damage_region,
                                                 int64_t                 encode_us,
                                                 size_t                  payload_bytes,
                                                 GError                **error);

typedef void (*MrdRdpGraphicsPipelineFrameAckCallback) (MrdRdpGraphicsPipeline *pipeline,
                                                        uint32_t                frame_id,
                                                        void                   *user_data);

void mrd_rdp_graphics_pipeline_set_frame_ack_callback (MrdRdpGraphicsPipeline                 *pipeline,
                                                       MrdRdpGraphicsPipelineFrameAckCallback  callback,
                                                       void                                   *user_data);

/* TRUE after queueDepth=0xFFFFFFFF — caller must stop acquiring frames. */
gboolean mrd_rdp_graphics_pipeline_acks_suspended (MrdRdpGraphicsPipeline *pipeline);

/* Returns FALSE on apply failure; controller disables for rest of session. */
typedef gboolean (*MrdRdpGraphicsPipelineBitrateCallback) (MrdRdpGraphicsPipeline *pipeline,
                                                           int                     mbps,
                                                           void                   *user_data);

void mrd_rdp_graphics_pipeline_set_bitrate_callback (MrdRdpGraphicsPipeline                *pipeline,
                                                     MrdRdpGraphicsPipelineBitrateCallback  callback,
                                                     void                                  *user_data);

void mrd_rdp_graphics_pipeline_configure_adaptive (MrdRdpGraphicsPipeline *pipeline,
                                                   gboolean                enabled,
                                                   int                     initial_mbps,
                                                   int                     min_mbps,
                                                   int                     max_mbps,
                                                   guint                   target_occupancy);

void mrd_rdp_graphics_pipeline_record_occupancy (MrdRdpGraphicsPipeline *pipeline,
                                                 guint                   occupancy);

G_END_DECLS
