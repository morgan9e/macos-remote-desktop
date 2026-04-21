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

gboolean mrd_rdp_graphics_pipeline_submit_frame (MrdRdpGraphicsPipeline *pipeline,
                                                 uint16_t                surface_id,
                                                 MrdBitstream           *main_bitstream,
                                                 MrdBitstream           *aux_bitstream,
                                                 cairo_region_t         *damage_region,
                                                 GError                **error);

typedef void (*MrdRdpGraphicsPipelineFrameAckCallback) (MrdRdpGraphicsPipeline *pipeline,
                                                        uint32_t                frame_id,
                                                        void                   *user_data);
void mrd_rdp_graphics_pipeline_set_frame_ack_callback (MrdRdpGraphicsPipeline                 *pipeline,
                                                       MrdRdpGraphicsPipelineFrameAckCallback  callback,
                                                       void                                   *user_data);
G_END_DECLS