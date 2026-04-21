#pragma once

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#include "mrd-types.h"

typedef enum {
  MRD_CODEC_NONE,
  MRD_CODEC_GFX,
} MrdCodecType;

typedef struct _RdpPeerContext
{
  rdpContext rdp_context;

  MrdSessionRdp *session_rdp;

  MrdCodecType codec;

  gboolean activated;
  uint32_t frame_id;

  HANDLE vcm;
} RdpPeerContext;

#define MRD_RDP_PEER_CONTEXT(ctx) ((RdpPeerContext *)(ctx))
