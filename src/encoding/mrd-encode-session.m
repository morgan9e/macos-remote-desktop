#include "mrd-encode-session.h"
#include "../util/mrd-bitstream.h"

#include <gio/gio.h>
#include <winpr/handle.h>
#include <winpr/synch.h>

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Accelerate/Accelerate.h>

struct _MrdEncodeSession
{
  GObject parent;

  MrdRdpCodec codec;
  gboolean have_avc444;
  gboolean have_avc420;
  gboolean is_started;

  uint32_t width;
  uint32_t height;
  int framerate;

  VTCompressionSessionRef main_session;
  VTCompressionSessionRef aux_session;  /* AVC444 aux view */

  MrdEncodeSessionCallback callback;
  void *callback_user_data;

  int64_t frame_count;

  CVPixelBufferRef main_nv12_buffer;
  CVPixelBufferRef aux_nv12_buffer;

  /* BT.601 video-range. */
  vImage_ARGBToYpCbCr bgra_to_nv12_info;
  gboolean bgra_to_nv12_ready;

  GMutex output_lock;
  MrdBitstream *pending_main;
  MrdBitstream *pending_aux;
  gboolean output_ready;

  /* Async pipeline (output_lock): VT callback pushes ready/damage/submit_us;
   * session thread drains via mrd_encode_session_drain_ready. */
  GQueue *ready_queue;
  GQueue *pending_damage;
  GQueue *pending_submit_us;
  HANDLE  output_event;   /* manual-reset; signaled on ready push */

  GQueue *bitstream_pool;
};

G_DEFINE_TYPE (MrdEncodeSession, mrd_encode_session, G_TYPE_OBJECT)

/* Caller must hold output_lock. */
static MrdBitstream *
acquire_bitstream_locked (MrdEncodeSession *session, size_t min_capacity)
{
  MrdBitstream *bs = g_queue_pop_head (session->bitstream_pool);
  if (bs) {
    mrd_bitstream_clear (bs);
    if (bs->capacity < min_capacity) {
      bs->data = g_realloc (bs->data, min_capacity);
      bs->capacity = min_capacity;
    }
    return bs;
  }
  return mrd_bitstream_new (min_capacity);
}

/* Caller must hold output_lock. */
static void
release_bitstream_locked (MrdEncodeSession *session, MrdBitstream *bs)
{
  if (!bs) return;
  /* Pool bounded so it doesn't bloat. */
  if (g_queue_get_length (session->bitstream_pool) < 8) {
    mrd_bitstream_clear (bs);
    g_queue_push_tail (session->bitstream_pool, bs);
  } else {
    mrd_bitstream_free (bs);
  }
}

/* AVCC [4-byte length][NAL] → Annex B [00 00 00 01][NAL]. output_lock held. */
static MrdBitstream *
convert_avcc_to_annexb (MrdEncodeSession *session, CMSampleBufferRef sampleBuffer)
{
  CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription (sampleBuffer);
  CMBlockBufferRef block = CMSampleBufferGetDataBuffer (sampleBuffer);
  if (!format || !block) {
    g_warning ("convert_avcc_to_annexb: missing %s",
               !format ? "format description" : "data buffer");
    return NULL;
  }

  size_t total_length = CMBlockBufferGetDataLength (block);
  char *data = NULL;
  OSStatus dp_status = CMBlockBufferGetDataPointer (block, 0, NULL, &total_length, &data);
  if (dp_status != kCMBlockBufferNoErr || !data) {
    g_warning ("CMBlockBufferGetDataPointer failed: %d", (int) dp_status);
    return NULL;
  }

  /* Acquire output bitstream from pool (zero-alloc after warmup) */
  MrdBitstream *output = acquire_bitstream_locked (session, total_length + 1024);

  /* Start code for Annex B */
  static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};

  /* Check if this is a keyframe - if so, prepend SPS/PPS */
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray (sampleBuffer, false);
  BOOL isKeyframe = FALSE;
  if (attachments && CFArrayGetCount (attachments) > 0) {
    CFDictionaryRef dict = CFArrayGetValueAtIndex (attachments, 0);
    isKeyframe = !CFDictionaryContainsKey (dict, kCMSampleAttachmentKey_NotSync);
  }

  if (isKeyframe) {
    size_t paramSetCount = 0;
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex (format, 0, NULL, NULL, &paramSetCount, NULL);

    for (size_t i = 0; i < paramSetCount; i++) {
      const uint8_t *paramSet = NULL;
      size_t paramSetSize = 0;

      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex (format, i, &paramSet, &paramSetSize, NULL, NULL) == noErr) {
        mrd_bitstream_append (output, start_code, 4);
        mrd_bitstream_append (output, paramSet, paramSetSize);
      }
    }
  }

  int nalLengthSize = 4;
  CMVideoFormatDescriptionGetH264ParameterSetAtIndex (format, 0, NULL, NULL, NULL, &nalLengthSize);

  size_t offset = 0;
  while (offset < total_length) {
    /* big-endian NAL length. */
    uint32_t nalLength = 0;
    for (int i = 0; i < nalLengthSize; i++) {
      nalLength = (nalLength << 8) | ((uint8_t)data[offset + i]);
    }
    offset += nalLengthSize;

    if (nalLength > total_length - offset) {
      g_warning ("Invalid NAL unit length: %u (remaining: %zu)", nalLength, total_length - offset);
      break;
    }

    mrd_bitstream_append (output, start_code, 4);
    mrd_bitstream_append (output, (uint8_t *)data + offset, nalLength);
    offset += nalLength;
  }

  return output;
}

void
mrd_encoded_frame_free (MrdEncodedFrame *frame)
{
  if (!frame) return;
  if (frame->main_bs)        mrd_bitstream_free (frame->main_bs);
  if (frame->aux_bs)         mrd_bitstream_free (frame->aux_bs);
  if (frame->damage_region)  cairo_region_destroy (frame->damage_region);
  g_free (frame);
}

/* Returns bitstreams to the pool. Use this for drain_ready() frames. */
void
mrd_encode_session_release_frame (MrdEncodeSession *session,
                                  MrdEncodedFrame  *frame)
{
  if (!frame) return;
  if (session) {
    g_mutex_lock (&session->output_lock);
    release_bitstream_locked (session, frame->main_bs);
    release_bitstream_locked (session, frame->aux_bs);
    g_mutex_unlock (&session->output_lock);
    frame->main_bs = NULL;
    frame->aux_bs = NULL;
  }
  if (frame->damage_region)
    cairo_region_destroy (frame->damage_region);
  g_free (frame);
}

/* Sync path populates pending_main/aux; async path also pops a damage
 * region + packages an MrdEncodedFrame to the ready_queue. */
static void
compression_output_callback (void                    *outputCallbackRefCon,
                             void                    *sourceFrameRefCon,
                             OSStatus                 status,
                             VTEncodeInfoFlags        infoFlags,
                             CMSampleBufferRef        sampleBuffer)
{
  MrdEncodeSession *session = (MrdEncodeSession *)outputCallbackRefCon;
  gboolean is_aux = (sourceFrameRefCon != NULL);
  (void)infoFlags;

  if (status != noErr) {
    g_warning ("Encoding failed: %d", (int)status);
    /* Drop matching pending entries to stay in sync. */
    if (!is_aux) {
      g_mutex_lock (&session->output_lock);
      if (!g_queue_is_empty (session->pending_damage)) {
        cairo_region_t *r = g_queue_pop_head (session->pending_damage);
        if (r) cairo_region_destroy (r);
      }
      if (!g_queue_is_empty (session->pending_submit_us))
        g_queue_pop_head (session->pending_submit_us);
      g_mutex_unlock (&session->output_lock);
    }
    return;
  }

  if (!CMSampleBufferDataIsReady (sampleBuffer)) {
    g_warning ("Sample buffer not ready");
    return;
  }

  g_mutex_lock (&session->output_lock);

  MrdBitstream *bitstream = convert_avcc_to_annexb (session, sampleBuffer);

  /* Sync-path slots. */
  if (is_aux) {
    g_clear_pointer (&session->pending_aux, mrd_bitstream_free);
    session->pending_aux = bitstream;
  } else {
    g_clear_pointer (&session->pending_main, mrd_bitstream_free);
    session->pending_main = bitstream;
  }
  if (session->pending_main) {
    if (session->codec == MRD_RDP_CODEC_AVC444v2)
      session->output_ready = (session->pending_aux != NULL);
    else
      session->output_ready = TRUE;
  }

  /* Deliver on main only; AVC444 aux was buffered above. */
  gboolean delivered = FALSE;
  /* NULL bitstream → drop damage instead of sending an empty frame. */
  if (!is_aux && !session->pending_main && !g_queue_is_empty (session->pending_damage)) {
    cairo_region_t *damage = g_queue_pop_head (session->pending_damage);
    if (damage)
      cairo_region_destroy (damage);
    if (!g_queue_is_empty (session->pending_submit_us))
      g_queue_pop_head (session->pending_submit_us);
  } else if (!is_aux && !g_queue_is_empty (session->pending_damage)) {
    cairo_region_t *damage = g_queue_pop_head (session->pending_damage);

    int64_t encode_us = 0;
    if (!g_queue_is_empty (session->pending_submit_us)) {
      int64_t submit_us = (int64_t) (gssize)
        g_queue_pop_head (session->pending_submit_us);
      encode_us = g_get_monotonic_time () - submit_us;
    }

    MrdEncodedFrame *frame = g_new0 (MrdEncodedFrame, 1);
    frame->main_bs = session->pending_main;
    frame->aux_bs  = session->pending_aux;
    frame->damage_region = damage;
    frame->encode_us = encode_us;
    frame->payload_bytes = (frame->main_bs ? mrd_bitstream_get_length (frame->main_bs) : 0)
                         + (frame->aux_bs  ? mrd_bitstream_get_length (frame->aux_bs)  : 0);
    session->pending_main = NULL;
    session->pending_aux  = NULL;
    session->output_ready = FALSE;

    g_queue_push_tail (session->ready_queue, frame);
    delivered = TRUE;
  }

  g_mutex_unlock (&session->output_lock);

  if (delivered)
    SetEvent (session->output_event);

  g_debug ("Encoded %s frame: %zu bytes (Annex B)",
           is_aux ? "aux" : "main", mrd_bitstream_get_length (bitstream));
}

static int
resolve_initial_bitrate_mbps (void)
{
  static gsize bitrate_init = 0;
  static int bitrate_mbps = 20;
  if (g_once_init_enter (&bitrate_init)) {
    const char *env = g_getenv ("MRD_BITRATE_MBPS");
    if (env && env[0] != '\0') {
      char *end = NULL;
      long v = strtol (env, &end, 10);
      if (end && *end == '\0' && v >= 0 && v <= 1000)
        bitrate_mbps = (int) v;
    }
    g_message ("VT bitrate cap: %d Mbps avg (%s)",
               bitrate_mbps,
               bitrate_mbps == 0 ? "uncapped" : "peak ~1.5x over 1s");
    g_once_init_leave (&bitrate_init, 1);
  }
  return bitrate_mbps;
}

int
mrd_encode_session_get_initial_bitrate_mbps (void)
{
  return resolve_initial_bitrate_mbps ();
}

static gboolean
apply_bitrate_to_session (VTCompressionSessionRef vt_session, int mbps)
{
  if (!vt_session)
    return FALSE;

  /* No "clear" API; uncapped just leaves whatever was last set. */
  if (mbps <= 0)
    return TRUE;

  int avg_bps = mbps * 1000 * 1000;
  OSStatus st = VTSessionSetProperty (vt_session,
                                      kVTCompressionPropertyKey_AverageBitRate,
                                      (__bridge CFTypeRef)@(avg_bps));
  if (st != noErr) {
    g_warning ("VT AverageBitRate=%d Mbps rejected: %d", mbps, (int)st);
    return FALSE;
  }

  int peak_bytes_per_sec = (int)((int64_t) avg_bps * 3 / 2 / 8);
  NSArray *limits = @[ @(peak_bytes_per_sec), @(1.0) ];
  st = VTSessionSetProperty (vt_session,
                             kVTCompressionPropertyKey_DataRateLimits,
                             (__bridge CFArrayRef)limits);
  if (st != noErr) {
    g_warning ("VT DataRateLimits rejected: %d", (int)st);
    return FALSE;
  }

  return TRUE;
}

static VTCompressionSessionRef
create_compression_session (int width, int height, int framerate, void *callback_ref)
{
  VTCompressionSessionRef session = NULL;
  OSStatus status;

  NSDictionary *encoderSpec = @{
    (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
  };

  NSDictionary *sourceImageAttrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (id)kCVPixelBufferWidthKey: @(width),
    (id)kCVPixelBufferHeightKey: @(height),
    (id)kCVPixelBufferMetalCompatibilityKey: @YES,
  };

  status = VTCompressionSessionCreate (
    NULL,                           /* allocator */
    width,
    height,
    kCMVideoCodecType_H264,
    (__bridge CFDictionaryRef)encoderSpec,
    (__bridge CFDictionaryRef)sourceImageAttrs,
    NULL,                           /* compressedDataAllocator */
    compression_output_callback,
    callback_ref,
    &session
  );

  if (status != noErr) {
    g_warning ("Failed to create compression session: %d", (int)status);
    return NULL;
  }

  VTSessionSetProperty (session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty (session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  /* VT buffers 2-4 frames by default — bound to one frame of delay. */
  VTSessionSetProperty (session, kVTCompressionPropertyKey_MaxFrameDelayCount,
                        (__bridge CFTypeRef)@(1));
  /* ~10s IDR interval. Large IDRs caused multi-hundred-ms RTT spikes;
   * recovery from loss is slower but FRAME_ACK flow control bounds drift. */
  VTSessionSetProperty (session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFTypeRef)@(framerate * 10));

  VTSessionSetProperty (session, kVTCompressionPropertyKey_ProfileLevel,
                        kVTProfileLevel_H264_High_AutoLevel);

  /* Without an explicit cap, motion frames balloon and saturate the wire. */
  apply_bitrate_to_session (session, resolve_initial_bitrate_mbps ());

  status = VTCompressionSessionPrepareToEncodeFrames (session);
  if (status != noErr) {
    g_warning ("Failed to prepare compression session: %d", (int)status);
    VTCompressionSessionInvalidate (session);
    CFRelease (session);
    return NULL;
  }

  return session;
}

static gboolean
init_bgra_to_nv12_info (MrdEncodeSession *session)
{
  vImage_YpCbCrPixelRange pixelRange = {
    .Yp_bias       = 16,
    .CbCr_bias     = 128,
    .YpRangeMax    = 235,
    .CbCrRangeMax  = 240,
    .YpMax         = 255,
    .YpMin         = 0,
    .CbCrMax       = 255,
    .CbCrMin       = 1,
  };

  vImage_Error err = vImageConvert_ARGBToYpCbCr_GenerateConversion (
    kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4,
    &pixelRange,
    &session->bgra_to_nv12_info,
    kvImageARGB8888,
    kvImage420Yp8_CbCr8,
    kvImageNoFlags);

  if (err != kvImageNoError) {
    g_warning ("vImageConvert_ARGBToYpCbCr_GenerateConversion failed: %ld", err);
    return FALSE;
  }

  session->bgra_to_nv12_ready = TRUE;
  return TRUE;
}

static void
convert_bgra_to_nv12 (MrdEncodeSession *session,
                      void             *bgra_data,
                      int               stride,
                      CVPixelBufferRef  nv12_buffer,
                      int               width,
                      int               height)
{
  CVPixelBufferLockBaseAddress (nv12_buffer, 0);

  vImage_Buffer src = {
    .data     = bgra_data,
    .width    = (vImagePixelCount)width,
    .height   = (vImagePixelCount)height,
    .rowBytes = (size_t)stride,
  };

  vImage_Buffer dest_y = {
    .data     = CVPixelBufferGetBaseAddressOfPlane (nv12_buffer, 0),
    .width    = (vImagePixelCount)width,
    .height   = (vImagePixelCount)height,
    .rowBytes = CVPixelBufferGetBytesPerRowOfPlane (nv12_buffer, 0),
  };

  vImage_Buffer dest_cbcr = {
    .data     = CVPixelBufferGetBaseAddressOfPlane (nv12_buffer, 1),
    .width    = (vImagePixelCount)(width / 2),
    .height   = (vImagePixelCount)(height / 2),
    .rowBytes = CVPixelBufferGetBytesPerRowOfPlane (nv12_buffer, 1),
  };

  /* BGRA {B,G,R,A} → ARGB {A,R,G,B}. */
  static const uint8_t permuteMap[4] = { 3, 2, 1, 0 };

  vImage_Error err = vImageConvert_ARGB8888To420Yp8_CbCr8 (
    &src, &dest_y, &dest_cbcr,
    &session->bgra_to_nv12_info,
    permuteMap,
    kvImageNoFlags);

  if (err != kvImageNoError)
    g_warning ("vImageConvert_ARGB8888To420Yp8_CbCr8 failed: %ld", err);

  CVPixelBufferUnlockBaseAddress (nv12_buffer, 0);
}

/* Public API */

MrdEncodeSession *
mrd_encode_session_new (gboolean have_avc444,
                        gboolean have_avc420)
{
  MrdEncodeSession *session = g_object_new (MRD_TYPE_ENCODE_SESSION, NULL);

  session->have_avc444 = have_avc444;
  session->have_avc420 = have_avc420;

  if (have_avc444)
    session->codec = MRD_RDP_CODEC_AVC444v2;
  else if (have_avc420)
    session->codec = MRD_RDP_CODEC_AVC420;
  else
    session->codec = MRD_RDP_CODEC_CAPROGRESSIVE;

  return session;
}

gboolean
mrd_encode_session_start (MrdEncodeSession  *session,
                          uint32_t           width,
                          uint32_t           height,
                          GError           **error)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), FALSE);

  if (session->is_started)
    return TRUE;

  session->width = width;
  session->height = height;
  session->framerate = 60;

  if (!init_bgra_to_nv12_info (session)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to build BGRA→NV12 conversion info");
    return FALSE;
  }

  session->main_session = create_compression_session (width, height, session->framerate, session);
  if (!session->main_session) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to create main encoder session");
    return FALSE;
  }

  if (session->codec == MRD_RDP_CODEC_AVC444v2) {
    session->aux_session = create_compression_session (width, height, session->framerate, session);
    if (!session->aux_session) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create auxiliary encoder session");
      VTCompressionSessionInvalidate (session->main_session);
      CFRelease (session->main_session);
      session->main_session = NULL;
      return FALSE;
    }
  }

  NSDictionary *attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (id)kCVPixelBufferWidthKey: @(width),
    (id)kCVPixelBufferHeightKey: @(height),
    (id)kCVPixelBufferMetalCompatibilityKey: @YES,
  };

  CVReturn ret = CVPixelBufferCreate (NULL, width, height,
                                      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                      (__bridge CFDictionaryRef)attrs,
                                      &session->main_nv12_buffer);
  if (ret != kCVReturnSuccess) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to create main NV12 buffer");
    return FALSE;
  }

  if (session->codec == MRD_RDP_CODEC_AVC444v2) {
    ret = CVPixelBufferCreate (NULL, width, height,
                               kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                               (__bridge CFDictionaryRef)attrs,
                               &session->aux_nv12_buffer);
    if (ret != kCVReturnSuccess) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create auxiliary NV12 buffer");
      return FALSE;
    }
  }

  session->is_started = TRUE;
  g_message ("Encoder started: %ux%u @ %d fps, codec=%d",
             width, height, session->framerate, session->codec);

  return TRUE;
}

void
mrd_encode_session_stop (MrdEncodeSession *session)
{
  g_return_if_fail (MRD_IS_ENCODE_SESSION (session));

  if (!session->is_started)
    return;

  if (session->main_session) {
    VTCompressionSessionCompleteFrames (session->main_session, kCMTimeInvalid);
    VTCompressionSessionInvalidate (session->main_session);
    CFRelease (session->main_session);
    session->main_session = NULL;
  }

  if (session->aux_session) {
    VTCompressionSessionCompleteFrames (session->aux_session, kCMTimeInvalid);
    VTCompressionSessionInvalidate (session->aux_session);
    CFRelease (session->aux_session);
    session->aux_session = NULL;
  }

  if (session->main_nv12_buffer) {
    CVPixelBufferRelease (session->main_nv12_buffer);
    session->main_nv12_buffer = NULL;
  }

  if (session->aux_nv12_buffer) {
    CVPixelBufferRelease (session->aux_nv12_buffer);
    session->aux_nv12_buffer = NULL;
  }

  g_mutex_lock (&session->output_lock);
  g_clear_pointer (&session->pending_main, mrd_bitstream_free);
  g_clear_pointer (&session->pending_aux, mrd_bitstream_free);
  session->output_ready = FALSE;
  g_mutex_unlock (&session->output_lock);

  session->is_started = FALSE;
  g_message ("Encoder stopped");
}

/* Caller must clear pending output first. */
static gboolean
encode_nv12_buffer (MrdEncodeSession  *session,
                    CVPixelBufferRef   nv12_buffer,
                    MrdBitstream     **out_main,
                    MrdBitstream     **out_aux,
                    GError           **error)
{
  CMTime pts = CMTimeMake (session->frame_count, session->framerate);
  CMTime duration = CMTimeMake (1, session->framerate);

  OSStatus status = VTCompressionSessionEncodeFrame (
    session->main_session, nv12_buffer, pts, duration,
    NULL, NULL, NULL);

  if (status != noErr) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to encode main frame: %d", (int)status);
    return FALSE;
  }

  if (session->codec == MRD_RDP_CODEC_AVC444v2 && session->aux_session) {
    /* TODO: proper auxiliary view (chroma difference). For now, main again. */
    status = VTCompressionSessionEncodeFrame (
      session->aux_session, nv12_buffer, pts, duration,
      NULL, (void *)1, NULL);
    if (status != noErr)
      g_warning ("Failed to encode auxiliary frame: %d", (int)status);
  }

  /* CompleteFrames(pts) blocks only on this pts; later frames stay in flight. */
  VTCompressionSessionCompleteFrames (session->main_session, pts);
  if (session->aux_session)
    VTCompressionSessionCompleteFrames (session->aux_session, pts);

  g_mutex_lock (&session->output_lock);
  if (session->pending_main) {
    *out_main = session->pending_main;
    session->pending_main = NULL;
  }
  if (out_aux && session->pending_aux) {
    *out_aux = session->pending_aux;
    session->pending_aux = NULL;
  }
  g_mutex_unlock (&session->output_lock);

  if (!*out_main) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No encoded output received");
    return FALSE;
  }

  session->frame_count++;
  return TRUE;
}

gboolean
mrd_encode_session_encode_frame (MrdEncodeSession  *session,
                                 const uint8_t     *pixel_data,
                                 uint32_t           width,
                                 uint32_t           height,
                                 uint32_t           stride,
                                 MrdBitstream     **out_main,
                                 MrdBitstream     **out_aux,
                                 GError           **error)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), FALSE);
  g_return_val_if_fail (session->is_started, FALSE);
  g_return_val_if_fail (out_main != NULL, FALSE);

  *out_main = NULL;
  if (out_aux)
    *out_aux = NULL;

  if (width != session->width || height != session->height) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                 "Frame size %ux%u doesn't match encoder %ux%u",
                 width, height, session->width, session->height);
    return FALSE;
  }

  g_mutex_lock (&session->output_lock);
  g_clear_pointer (&session->pending_main, mrd_bitstream_free);
  g_clear_pointer (&session->pending_aux, mrd_bitstream_free);
  session->output_ready = FALSE;
  g_mutex_unlock (&session->output_lock);

  convert_bgra_to_nv12 (session, (void *)pixel_data, stride,
                        session->main_nv12_buffer,
                        session->width, session->height);
  if (session->codec == MRD_RDP_CODEC_AVC444v2 && session->aux_session) {
    convert_bgra_to_nv12 (session, (void *)pixel_data, stride,
                          session->aux_nv12_buffer,
                          session->width, session->height);
    return encode_nv12_buffer (session, session->aux_nv12_buffer,
                               out_main, out_aux, error);
  }

  return encode_nv12_buffer (session, session->main_nv12_buffer,
                             out_main, out_aux, error);
}

gboolean
mrd_encode_session_encode_pixel_buffer (MrdEncodeSession  *session,
                                        void              *pixel_buffer,
                                        MrdBitstream     **out_main,
                                        MrdBitstream     **out_aux,
                                        GError           **error)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), FALSE);
  g_return_val_if_fail (session->is_started, FALSE);
  g_return_val_if_fail (pixel_buffer != NULL, FALSE);
  g_return_val_if_fail (out_main != NULL, FALSE);

  *out_main = NULL;
  if (out_aux)
    *out_aux = NULL;

  CVPixelBufferRef pb = (CVPixelBufferRef)pixel_buffer;
  size_t pb_w = CVPixelBufferGetWidth (pb);
  size_t pb_h = CVPixelBufferGetHeight (pb);
  if (pb_w != session->width || pb_h != session->height) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                 "Pixel buffer %zux%zu doesn't match encoder %ux%u",
                 pb_w, pb_h, session->width, session->height);
    return FALSE;
  }

  g_mutex_lock (&session->output_lock);
  g_clear_pointer (&session->pending_main, mrd_bitstream_free);
  g_clear_pointer (&session->pending_aux, mrd_bitstream_free);
  session->output_ready = FALSE;
  g_mutex_unlock (&session->output_lock);

  return encode_nv12_buffer (session, pb, out_main, out_aux, error);
}

gboolean
mrd_encode_session_submit_pixel_buffer (MrdEncodeSession  *session,
                                        void              *pixel_buffer,
                                        cairo_region_t    *damage_region,
                                        GError           **error)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), FALSE);
  g_return_val_if_fail (session->is_started, FALSE);
  g_return_val_if_fail (pixel_buffer != NULL, FALSE);

  CVPixelBufferRef pb = (CVPixelBufferRef)pixel_buffer;
  size_t pb_w = CVPixelBufferGetWidth (pb);
  size_t pb_h = CVPixelBufferGetHeight (pb);
  if (pb_w != session->width || pb_h != session->height) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                 "Pixel buffer %zux%zu doesn't match encoder %ux%u",
                 pb_w, pb_h, session->width, session->height);
    if (damage_region) cairo_region_destroy (damage_region);
    return FALSE;
  }

  CMTime pts = CMTimeMake (session->frame_count, session->framerate);
  CMTime duration = CMTimeMake (1, session->framerate);

  /* AllowFrameReordering=false → callbacks fire in submit order; FIFO is safe. */
  int64_t submit_us = g_get_monotonic_time ();
  g_mutex_lock (&session->output_lock);
  g_queue_push_tail (session->pending_damage, damage_region);
  g_queue_push_tail (session->pending_submit_us,
                     (gpointer) (gssize) submit_us);
  g_mutex_unlock (&session->output_lock);

  OSStatus status = VTCompressionSessionEncodeFrame (
    session->main_session, pb, pts, duration,
    NULL, NULL, NULL);

  if (status != noErr) {
    /* Roll back queued entries — callback won't fire. */
    g_mutex_lock (&session->output_lock);
    cairo_region_t *r = g_queue_pop_tail (session->pending_damage);
    g_queue_pop_tail (session->pending_submit_us);
    g_mutex_unlock (&session->output_lock);
    if (r) cairo_region_destroy (r);

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "VTCompressionSessionEncodeFrame failed: %d", (int)status);
    return FALSE;
  }

  if (session->codec == MRD_RDP_CODEC_AVC444v2 && session->aux_session) {
    /* TODO: proper aux view; for now, encode same buffer. */
    status = VTCompressionSessionEncodeFrame (
      session->aux_session, pb, pts, duration,
      NULL, (void *)1, NULL);
    if (status != noErr)
      g_warning ("Aux VTCompressionSessionEncodeFrame failed: %d", (int)status);
  }

  session->frame_count++;
  return TRUE;
}

guint
mrd_encode_session_drain_ready (MrdEncodeSession *session,
                                GQueue           *out_frames)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), 0);
  g_return_val_if_fail (out_frames != NULL, 0);

  guint n = 0;
  g_mutex_lock (&session->output_lock);
  MrdEncodedFrame *f;
  while ((f = g_queue_pop_head (session->ready_queue)) != NULL) {
    g_queue_push_tail (out_frames, f);
    n++;
  }
  /* Reset under lock to avoid race with concurrent push. */
  if (session->output_event)
    ResetEvent (session->output_event);
  g_mutex_unlock (&session->output_lock);

  return n;
}

HANDLE
mrd_encode_session_get_output_event_handle (MrdEncodeSession *session)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), NULL);
  return session->output_event;
}

gboolean
mrd_encode_session_set_bitrate_mbps (MrdEncodeSession *session, int mbps)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), FALSE);

  if (!session->is_started || !session->main_session)
    return FALSE;

  if (!apply_bitrate_to_session (session->main_session, mbps))
    return FALSE;

  if (session->aux_session)
    {
      /* Best-effort on aux. */
      if (!apply_bitrate_to_session (session->aux_session, mbps))
        g_warning ("VT bitrate update failed on aux session (main was OK)");
    }

  return TRUE;
}

guint
mrd_encode_session_get_outstanding (MrdEncodeSession *session)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), 0);

  g_mutex_lock (&session->output_lock);
  guint n = g_queue_get_length (session->pending_damage)
          + g_queue_get_length (session->ready_queue);
  g_mutex_unlock (&session->output_lock);
  return n;
}

void
mrd_encode_session_set_callback (MrdEncodeSession         *session,
                                 MrdEncodeSessionCallback  callback,
                                 void                     *user_data)
{
  g_return_if_fail (MRD_IS_ENCODE_SESSION (session));

  session->callback = callback;
  session->callback_user_data = user_data;
}

MrdRdpCodec
mrd_encode_session_get_codec (MrdEncodeSession *session)
{
  g_return_val_if_fail (MRD_IS_ENCODE_SESSION (session), MRD_RDP_CODEC_CAPROGRESSIVE);
  return session->codec;
}

static void
mrd_encode_session_finalize (GObject *object)
{
  MrdEncodeSession *session = MRD_ENCODE_SESSION (object);

  mrd_encode_session_stop (session);

  if (session->ready_queue) {
    g_queue_free_full (session->ready_queue,
                       (GDestroyNotify)mrd_encoded_frame_free);
    session->ready_queue = NULL;
  }
  if (session->pending_damage) {
    g_queue_free_full (session->pending_damage,
                       (GDestroyNotify)cairo_region_destroy);
    session->pending_damage = NULL;
  }
  if (session->pending_submit_us) {
    g_queue_free (session->pending_submit_us);
    session->pending_submit_us = NULL;
  }
  if (session->bitstream_pool) {
    g_queue_free_full (session->bitstream_pool,
                       (GDestroyNotify)mrd_bitstream_free);
    session->bitstream_pool = NULL;
  }
  if (session->output_event) {
    CloseHandle (session->output_event);
    session->output_event = NULL;
  }

  g_mutex_clear (&session->output_lock);

  G_OBJECT_CLASS (mrd_encode_session_parent_class)->finalize (object);
}

static void
mrd_encode_session_init (MrdEncodeSession *session)
{
  g_mutex_init (&session->output_lock);
  session->ready_queue = g_queue_new ();
  session->pending_damage = g_queue_new ();
  session->pending_submit_us = g_queue_new ();
  session->bitstream_pool = g_queue_new ();
  session->output_event = CreateEvent (NULL, TRUE, FALSE, NULL);
}

static void
mrd_encode_session_class_init (MrdEncodeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mrd_encode_session_finalize;
}
