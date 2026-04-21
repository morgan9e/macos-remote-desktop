#pragma once
#include <glib.h>

typedef struct _MrdContext MrdContext;
typedef struct _MrdSessionRdp MrdSessionRdp;
typedef struct _MrdRdpServer MrdRdpServer;

typedef struct _MrdEncodeSession MrdEncodeSession;
typedef struct _MrdEncodeContext MrdEncodeContext;
typedef struct _MrdHwAccelMetal MrdHwAccelMetal;

typedef struct _MrdRdpFrame MrdRdpFrame;
typedef struct _MrdRdpGfxSurface MrdRdpGfxSurface;
typedef struct _MrdRdpRenderContext MrdRdpRenderContext;
typedef struct _MrdRdpRenderer MrdRdpRenderer;
typedef struct _MrdRdpGraphicsPipeline MrdRdpGraphicsPipeline;

typedef struct _MrdRdpBuffer MrdRdpBuffer;
typedef struct _MrdRdpBufferPool MrdRdpBufferPool;
typedef struct _MrdImageView MrdImageView;
typedef struct _MrdBitstream MrdBitstream;

typedef struct _MrdScreenCapture MrdScreenCapture;
typedef struct _MrdInputInjector MrdInputInjector;

typedef enum _MrdPixelFormat
{
  MRD_PIXEL_FORMAT_BGRA8888,
  MRD_PIXEL_FORMAT_NV12,
} MrdPixelFormat;

typedef enum _MrdRdpCodec
{
  MRD_RDP_CODEC_CAPROGRESSIVE,
  MRD_RDP_CODEC_AVC420,
  MRD_RDP_CODEC_AVC444v2,
} MrdRdpCodec;

typedef enum _MrdRdpFrameViewType
{
  MRD_RDP_FRAME_VIEW_TYPE_MAIN,
  MRD_RDP_FRAME_VIEW_TYPE_AUX,
  MRD_RDP_FRAME_VIEW_TYPE_DUAL,
} MrdRdpFrameViewType;