#pragma once
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/codec/nsc.h>
#include "mrd-types.h"
typedef enum {
  MRD_CODEC_NONE,
  MRD_CODEC_RFX,
  MRD_CODEC_NSC,
  MRD_CODEC_GFX,
} MrdCodecType;
typedef struct _RdpPeerContext
{
  rdpContext rdp_context;
  MrdSessionRdp *session_rdp;
  RFX_CONTEXT *rfx_context;
  NSC_CONTEXT *nsc_context;
  MrdCodecType codec;
  wStream *encode_stream;
  gboolean activated;
  uint32_t frame_id;
  
  HANDLE vcm;
  GMutex channel_mutex;
  
  MrdRdpGraphicsPipeline *graphics_pipeline;
  
  MrdScreenCapture *screen_capture;
  MrdInputInjector *input_injector;
  
  MrdEncodeSession *encode_session;
  MrdHwAccelMetal *hwaccel_metal;
} RdpPeerContext;

#define MRD_RDP_PEER_CONTEXT(ctx) ((RdpPeerContext *)(ctx))