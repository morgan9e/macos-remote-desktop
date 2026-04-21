#include "mrd-encode-session.h"
#include "../util/mrd-bitstream.h"
#include <gio/gio.h>
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
  VTCompressionSessionRef aux_session;  
  
  MrdEncodeSessionCallback callback;
  void *callback_user_data;
  
  int64_t frame_count;
  
  CVPixelBufferRef main_nv12_buffer;
  CVPixelBufferRef aux_nv12_buffer;
  
  vImage_ARGBToYpCbCr bgra_to_nv12_info;
  gboolean bgra_to_nv12_ready;
  
  GMutex output_lock;
  MrdBitstream *pending_main;
  MrdBitstream *pending_aux;
  gboolean output_ready;
};
G_DEFINE_TYPE (MrdEncodeSession, mrd_encode_session, G_TYPE_OBJECT)

static MrdBitstream *
convert_avcc_to_annexb (CMSampleBufferRef sampleBuffer)
{
  CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription (sampleBuffer);
  CMBlockBufferRef block = CMSampleBufferGetDataBuffer (sampleBuffer);
  size_t total_length = CMBlockBufferGetDataLength (block);
  char *data = NULL;
  CMBlockBufferGetDataPointer (block, 0, NULL, &total_length, &data);
  
  MrdBitstream *output = mrd_bitstream_new (total_length + 1024);
  
  static const uint8_t start_code[] = {0x00, 0x00, 0x00, 0x01};
  
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

static void
compression_output_callback (void                    *outputCallbackRefCon,
                             void                    *sourceFrameRefCon,
                             OSStatus                 status,
                             VTEncodeInfoFlags        infoFlags,
                             CMSampleBufferRef        sampleBuffer)
{
  MrdEncodeSession *session = (MrdEncodeSession *)outputCallbackRefCon;
  gboolean is_aux = (sourceFrameRefCon != NULL);
  if (status != noErr) {
    g_warning ("Encoding failed: %d", (int)status);
    return;
  }
  if (!CMSampleBufferDataIsReady (sampleBuffer)) {
    g_warning ("Sample buffer not ready");
    return;
  }
  
  MrdBitstream *bitstream = convert_avcc_to_annexb (sampleBuffer);
  g_mutex_lock (&session->output_lock);
  if (is_aux) {
    g_clear_pointer (&session->pending_aux, mrd_bitstream_free);
    session->pending_aux = bitstream;
  } else {
    g_clear_pointer (&session->pending_main, mrd_bitstream_free);
    session->pending_main = bitstream;
  }
  
  if (session->pending_main) {
    if (session->codec == MRD_RDP_CODEC_AVC444v2) {
      session->output_ready = (session->pending_aux != NULL);
    } else {
      session->output_ready = TRUE;
    }
  }
  g_mutex_unlock (&session->output_lock);
  g_debug ("Encoded %s frame: %zu bytes (Annex B)",
           is_aux ? "aux" : "main", mrd_bitstream_get_length (bitstream));
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
    NULL,                           
    width,
    height,
    kCMVideoCodecType_H264,
    (__bridge CFDictionaryRef)encoderSpec,
    (__bridge CFDictionaryRef)sourceImageAttrs,
    NULL,                           
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
  VTSessionSetProperty (session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFTypeRef)@(framerate * 2));
  
  VTSessionSetProperty (session, kVTCompressionPropertyKey_ProfileLevel,
                        kVTProfileLevel_H264_Main_AutoLevel);
  
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
    
    status = VTCompressionSessionEncodeFrame (
      session->aux_session, nv12_buffer, pts, duration,
      NULL, (void *)1, NULL);
    if (status != noErr)
      g_warning ("Failed to encode auxiliary frame: %d", (int)status);
  }
  
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
  g_mutex_clear (&session->output_lock);
  G_OBJECT_CLASS (mrd_encode_session_parent_class)->finalize (object);
}
static void
mrd_encode_session_init (MrdEncodeSession *session)
{
  g_mutex_init (&session->output_lock);
}
static void
mrd_encode_session_class_init (MrdEncodeSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_encode_session_finalize;
}