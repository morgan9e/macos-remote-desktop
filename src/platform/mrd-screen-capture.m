#include "mrd-screen-capture.h"

#include <math.h>
#include <gio/gio.h>
#include <winpr/synch.h>

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreAudioTypes/CoreAudioBaseTypes.h>

@interface MrdStreamOutput : NSObject <SCStreamOutput>
{
  MrdScreenCapture *_capture;
}
- (instancetype)initWithCapture:(MrdScreenCapture *)capture;
- (void)handleAudioSample:(CMSampleBufferRef)sampleBuffer;
@end

struct _MrdScreenCapture
{
  GObject parent;

  SCStream *stream;
  SCStreamConfiguration *config;
  MrdStreamOutput *output;

  MrdScreenCaptureFrameCallback frame_callback;
  void *callback_user_data;

  int width;
  int height;
  float scale_factor;

  gboolean is_running;

  uint32_t target_display_id;

  /* 0 → source resolution. */
  uint32_t output_width;
  uint32_t output_height;
  gboolean scales_to_fit;

  GMutex frame_lock;
  uint8_t *frame_buffer;
  uint32_t frame_width;
  uint32_t frame_height;
  uint32_t frame_stride;
  gboolean frame_ready;

  /* nv12_mode: handler wraps CVPixelBuffer + dirty into latest_captured_frame
   * (no memcpy); take_captured_frame steals it. */
  gboolean nv12_mode;
  MrdCapturedFrame *latest_captured_frame;  /* owned, protected by frame_lock */

  /* Manual-reset; reset in take_captured_frame. */
  HANDLE frame_available_event;

  uint64_t damage_frame_counter;

  /* Audio sink: set/cleared from session thread, read from SCK audio queue. */
  MrdScreenCaptureAudioCallback audio_callback;
  void                         *audio_callback_user_data;

  /* Reusable s16 interleaved staging for audio (fills on callback thread). */
  int16_t *audio_stage;
  size_t   audio_stage_frames_capacity;
};

static void
release_captured_frame_locked (MrdCapturedFrame *frame)
{
  if (!frame)
    return;
  if (frame->pixel_buffer)
    CVPixelBufferRelease ((CVPixelBufferRef)frame->pixel_buffer);
  if (frame->dirty)
    cairo_region_destroy (frame->dirty);
  g_free (frame);
}

void
mrd_captured_frame_free (MrdCapturedFrame *frame)
{
  release_captured_frame_locked (frame);
}

G_DEFINE_TYPE (MrdScreenCapture, mrd_screen_capture, G_TYPE_OBJECT)

@implementation MrdStreamOutput

- (instancetype)initWithCapture:(MrdScreenCapture *)capture
{
  self = [super init];
  if (self) {
    _capture = capture;
  }
  return self;
}

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
  if (type == SCStreamOutputTypeAudio) {
    [self handleAudioSample:sampleBuffer];
    return;
  }
  if (type != SCStreamOutputTypeScreen)
    return;

  CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!imageBuffer)
    return;

  /* NV12 zero-copy: wrap pixel buffer + dirty rects, never read pixels. */
  if (_capture->nv12_mode) {
    CFArrayRef attachmentsArray =
      CMSampleBufferGetSampleAttachmentsArray (sampleBuffer, false);
    if (!attachmentsArray || CFArrayGetCount (attachmentsArray) == 0)
      return;

    CFDictionaryRef attachments =
      (CFDictionaryRef)CFArrayGetValueAtIndex (attachmentsArray, 0);

    SCFrameStatus status = SCFrameStatusComplete;
    CFNumberRef statusNum =
      CFDictionaryGetValue (attachments, (CFStringRef)SCStreamFrameInfoStatus);
    if (statusNum)
      CFNumberGetValue (statusNum, kCFNumberSInt32Type, &status);

    if (status != SCFrameStatusComplete)
      return;

    /* Dirty rects (array of CGRect-dictionaries) + scale factor. */
    CFArrayRef dirtyRectsArr = CFDictionaryGetValue (attachments,
        (CFStringRef)SCStreamFrameInfoDirtyRects);
    double scaleFactor = 1.0;
    CFNumberRef sfNum = CFDictionaryGetValue (attachments,
        (CFStringRef)SCStreamFrameInfoScaleFactor);
    if (sfNum)
      CFNumberGetValue (sfNum, kCFNumberDoubleType, &scaleFactor);

    size_t pb_w = CVPixelBufferGetWidth (imageBuffer);
    size_t pb_h = CVPixelBufferGetHeight (imageBuffer);

    /* SCK dirty rects miss some updates (subpixel text, cursor trails);
     * union with full-surface every MRD_DAMAGE_REFRESH frames to recover.
     * MRD_FULL_DAMAGE=1: every frame full. MRD_DAMAGE_REFRESH=0: pure SCK. */
    static gsize damage_init = 0;
    static gboolean force_full = FALSE;
    static int refresh_interval = 8;
    if (g_once_init_enter (&damage_init)) {
      const char *full_env = g_getenv ("MRD_FULL_DAMAGE");
      force_full = full_env && full_env[0] != '\0' && g_strcmp0 (full_env, "0") != 0;

      const char *refresh_env = g_getenv ("MRD_DAMAGE_REFRESH");
      if (refresh_env && refresh_env[0] != '\0') {
        char *end = NULL;
        long v = strtol (refresh_env, &end, 10);
        if (end && *end == '\0' && v >= 0 && v <= 600)
          refresh_interval = (int) v;
      }

      if (force_full)
        g_message ("MRD_FULL_DAMAGE=1 — full-surface damage every frame");
      else if (refresh_interval == 0)
        g_message ("MRD_DAMAGE_REFRESH=0 — pure SCK damage (no periodic refresh)");
      else
        g_message ("Damage: SCK rects + full-surface refresh every %d frames",
                   refresh_interval);

      g_once_init_leave (&damage_init, 1);
    }

    gboolean inject_full = force_full ||
      (refresh_interval > 0 && (_capture->damage_frame_counter % refresh_interval) == 0);
    _capture->damage_frame_counter++;

    cairo_region_t *dirty = cairo_region_create ();
    gboolean have_rects = FALSE;
    if (inject_full) {
      cairo_rectangle_int_t full = {
        .x = 0, .y = 0, .width = (int)pb_w, .height = (int)pb_h,
      };
      cairo_region_union_rectangle (dirty, &full);
      have_rects = TRUE;
    }
    if (!force_full && dirtyRectsArr) {
      CFIndex n = CFArrayGetCount (dirtyRectsArr);
      for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef rectDict =
          (CFDictionaryRef)CFArrayGetValueAtIndex (dirtyRectsArr, i);
        CGRect r;
        if (!CGRectMakeWithDictionaryRepresentation (rectDict, &r))
          continue;

        /* points → pixels: floor origin, ceil extents. */
        double x1 = floor (r.origin.x * scaleFactor);
        double y1 = floor (r.origin.y * scaleFactor);
        double x2 = ceil ((r.origin.x + r.size.width) * scaleFactor);
        double y2 = ceil ((r.origin.y + r.size.height) * scaleFactor);

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > (double)pb_w) x2 = (double)pb_w;
        if (y2 > (double)pb_h) y2 = (double)pb_h;
        if (x2 <= x1 || y2 <= y1)
          continue;

        cairo_rectangle_int_t ri = {
          .x = (int)x1, .y = (int)y1,
          .width = (int)(x2 - x1), .height = (int)(y2 - y1),
        };
        cairo_region_union_rectangle (dirty, &ri);
        have_rects = TRUE;
      }
    }

    /* Complete frame with no dirty rects → full-frame fallback. */
    if (!have_rects) {
      cairo_rectangle_int_t full = {
        .x = 0, .y = 0, .width = (int)pb_w, .height = (int)pb_h,
      };
      cairo_region_union_rectangle (dirty, &full);
    }

    MrdCapturedFrame *cf = g_new0 (MrdCapturedFrame, 1);
    CVPixelBufferRetain (imageBuffer);
    cf->pixel_buffer = imageBuffer;
    cf->dirty = dirty;

    g_mutex_lock (&_capture->frame_lock);
    release_captured_frame_locked (_capture->latest_captured_frame);
    _capture->latest_captured_frame = cf;
    _capture->frame_width  = (uint32_t)pb_w;
    _capture->frame_height = (uint32_t)pb_h;
    _capture->frame_ready = TRUE;
    g_mutex_unlock (&_capture->frame_lock);

    if (_capture->frame_available_event)
      SetEvent (_capture->frame_available_event);
    return;
  }

  CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

  void *baseAddress = CVPixelBufferGetBaseAddress(imageBuffer);
  size_t width = CVPixelBufferGetWidth(imageBuffer);
  size_t height = CVPixelBufferGetHeight(imageBuffer);
  size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);

  g_mutex_lock (&_capture->frame_lock);

  size_t frameSize = height * bytesPerRow;
  if (!_capture->frame_buffer ||
      _capture->frame_width != width ||
      _capture->frame_height != height ||
      _capture->frame_stride != bytesPerRow) {
    g_free (_capture->frame_buffer);
    _capture->frame_buffer = g_malloc (frameSize);
    _capture->frame_width = (uint32_t)width;
    _capture->frame_height = (uint32_t)height;
    _capture->frame_stride = (uint32_t)bytesPerRow;
  }

  memcpy (_capture->frame_buffer, baseAddress, frameSize);
  _capture->frame_ready = TRUE;

  g_mutex_unlock (&_capture->frame_lock);

  if (_capture->frame_callback) {
    _capture->frame_callback(_capture,
                             baseAddress,
                             (int)width,
                             (int)height,
                             (int)bytesPerRow,
                             _capture->callback_user_data);
  }

  CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

- (void)handleAudioSample:(CMSampleBufferRef)sampleBuffer
{
  if (!_capture->audio_callback)
    return;

  CMAudioFormatDescriptionRef fd = CMSampleBufferGetFormatDescription (sampleBuffer);
  if (!fd)
    return;
  const AudioStreamBasicDescription *asbd =
    CMAudioFormatDescriptionGetStreamBasicDescription (fd);
  if (!asbd)
    return;

  CMItemCount n_frames = CMSampleBufferGetNumSamples (sampleBuffer);
  if (n_frames <= 0)
    return;

  /* We only handle Float32 (interleaved or non-interleaved). */
  if (asbd->mFormatID != kAudioFormatLinearPCM ||
      !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) ||
      asbd->mBitsPerChannel != 32) {
    static gsize warn_once = 0;
    if (g_once_init_enter (&warn_once)) {
      g_warning ("Audio capture: unsupported format "
                 "(formatID=0x%x flags=0x%x bits=%u)",
                 (unsigned)asbd->mFormatID, (unsigned)asbd->mFormatFlags,
                 (unsigned)asbd->mBitsPerChannel);
      g_once_init_leave (&warn_once, 1);
    }
    return;
  }

  const UInt32 n_channels = asbd->mChannelsPerFrame;
  if (n_channels == 0 || n_channels > 8)
    return;

  /* Extract the audio buffer list, including the backing block buffer. */
  size_t abl_size = 0;
  OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
    sampleBuffer, &abl_size, NULL, 0, NULL, NULL, 0, NULL);
  if (st != noErr || abl_size == 0)
    return;

  AudioBufferList *abl = g_malloc (abl_size);
  CMBlockBufferRef block_buf = NULL;
  st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
    sampleBuffer, NULL, abl, abl_size,
    kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
    kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
    &block_buf);
  if (st != noErr || abl->mNumberBuffers == 0) {
    if (block_buf) CFRelease (block_buf);
    g_free (abl);
    return;
  }

  /* Grow staging (interleaved stereo s16). We downmix/upmix to 2ch. */
  size_t need_frames = (size_t) n_frames;
  if (_capture->audio_stage_frames_capacity < need_frames) {
    g_free (_capture->audio_stage);
    _capture->audio_stage = g_new (int16_t, need_frames * 2);
    _capture->audio_stage_frames_capacity = need_frames;
  }
  int16_t *out = _capture->audio_stage;

  const gboolean non_interleaved =
    (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;

  if (non_interleaved) {
    /* Each channel in its own mBuffers[i]. mNumberBuffers == mChannelsPerFrame. */
    const float *L = (const float *) abl->mBuffers[0].mData;
    const float *R = (n_channels >= 2 && abl->mNumberBuffers >= 2)
                     ? (const float *) abl->mBuffers[1].mData : L;
    for (size_t i = 0; i < need_frames; i++) {
      float l = L ? L[i] : 0.0f;
      float r = R ? R[i] : l;
      if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
      if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
      out[i * 2]     = (int16_t) lrintf (l * 32767.0f);
      out[i * 2 + 1] = (int16_t) lrintf (r * 32767.0f);
    }
  } else {
    /* Interleaved: all channels packed in mBuffers[0]. */
    const float *src = (const float *) abl->mBuffers[0].mData;
    if (!src) {
      if (block_buf) CFRelease (block_buf);
      g_free (abl);
      return;
    }
    for (size_t i = 0; i < need_frames; i++) {
      float l = src[i * n_channels];
      float r = (n_channels >= 2) ? src[i * n_channels + 1] : l;
      if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
      if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
      out[i * 2]     = (int16_t) lrintf (l * 32767.0f);
      out[i * 2 + 1] = (int16_t) lrintf (r * 32767.0f);
    }
  }

  /* Snapshot callback pointers under frame_lock — set_audio_callback rewrites
   * them from the session thread. */
  MrdScreenCaptureAudioCallback cb;
  void *ud;
  g_mutex_lock (&_capture->frame_lock);
  cb = _capture->audio_callback;
  ud = _capture->audio_callback_user_data;
  g_mutex_unlock (&_capture->frame_lock);
  if (cb)
    cb (out, need_frames, ud);

  if (block_buf) CFRelease (block_buf);
  g_free (abl);
}

@end

MrdScreenCapture *
mrd_screen_capture_new (void)
{
  return g_object_new (MRD_TYPE_SCREEN_CAPTURE, NULL);
}

MrdScreenCapture *
mrd_screen_capture_new_for_display (uint32_t display_id)
{
  MrdScreenCapture *capture = g_object_new (MRD_TYPE_SCREEN_CAPTURE, NULL);
  capture->target_display_id = display_id;
  return capture;
}

MrdScreenCapture *
mrd_screen_capture_new_scaled (uint32_t display_id,
                                uint32_t output_width,
                                uint32_t output_height)
{
  MrdScreenCapture *capture = g_object_new (MRD_TYPE_SCREEN_CAPTURE, NULL);
  capture->target_display_id = display_id;
  capture->output_width = output_width;
  capture->output_height = output_height;
  capture->scales_to_fit = TRUE;
  return capture;
}

gboolean
mrd_screen_capture_start (MrdScreenCapture  *capture,
                          GError           **error)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), FALSE);

  if (capture->is_running)
    return TRUE;

  @autoreleasepool {
    __block NSError *scError = nil;
    __block SCDisplay *primaryDisplay = nil;
    __block gboolean success = FALSE;

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *err) {
      if (err) {
        scError = err;
        dispatch_semaphore_signal(semaphore);
        return;
      }

      if (content.displays.count == 0) {
        scError = [NSError errorWithDomain:@"MrdScreenCapture"
                                     code:1
                                 userInfo:@{NSLocalizedDescriptionKey: @"No displays found"}];
        dispatch_semaphore_signal(semaphore);
        return;
      }

      SCDisplay *target = nil;
      for (SCDisplay *d in content.displays) {
        if (capture->target_display_id != 0 &&
            d.displayID == capture->target_display_id) {
          target = d;
          break;
        }
      }
      if (!target) {
        if (capture->target_display_id != 0) {
          scError = [NSError errorWithDomain:@"MrdScreenCapture"
                                       code:2
                                   userInfo:@{NSLocalizedDescriptionKey:
                                       [NSString stringWithFormat:
                                         @"Display %u not found",
                                         capture->target_display_id]}];
          dispatch_semaphore_signal(semaphore);
          return;
        }
        target = content.displays.firstObject;
      }
      primaryDisplay = target;
      dispatch_semaphore_signal(semaphore);
    }];

    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (scError) {
      if (error) {
        *error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Failed to get shareable content: %s",
                              scError.localizedDescription.UTF8String);
      }
      return FALSE;
    }

    capture->config = [[SCStreamConfiguration alloc] init];

    if (capture->scales_to_fit && capture->output_width > 0 && capture->output_height > 0)
      {
        capture->config.width = capture->output_width;
        capture->config.height = capture->output_height;
        capture->config.scalesToFit = YES;
        g_message ("Screen capture: scaling %lux%lu → %ux%u",
                   (unsigned long)primaryDisplay.width, (unsigned long)primaryDisplay.height,
                   capture->output_width, capture->output_height);
      }
    else
      {
        capture->config.width = primaryDisplay.width;
        capture->config.height = primaryDisplay.height;
        capture->config.scalesToFit = NO;
      }

    capture->config.pixelFormat = kCVPixelFormatType_32BGRA;
    /* Some clients (mstsc, mobile) ignore server PointerPosition over the
     * RDP window. MRD_CURSOR_IN_VIDEO=1 bakes cursor into H.264 instead. */
    {
      const char *cursor_in_video = g_getenv ("MRD_CURSOR_IN_VIDEO");
      gboolean show = cursor_in_video && cursor_in_video[0] != '\0' &&
                      g_strcmp0 (cursor_in_video, "0") != 0;
      capture->config.showsCursor = show ? YES : NO;
      if (show)
        g_message ("MRD_CURSOR_IN_VIDEO set — cursor will be rendered into the video stream");
    }
    /* Drop stale captures rather than buffer them under encode stalls. */
    capture->config.queueDepth = 2;
    /* Upper FPS cap; SCK only delivers on change so idle stays cheap. */
    capture->config.minimumFrameInterval = CMTimeMake(1, 120);

    /* Audio tap: same SCK grant covers it. Excluding this process prevents
     * a feedback loop if the server ever emits sound (beeps, diagnostic). */
    if (@available(macOS 13.0, *)) {
      capture->config.capturesAudio = YES;
      capture->config.sampleRate = 48000;
      capture->config.channelCount = 2;
      capture->config.excludesCurrentProcessAudio = YES;
    }

    capture->width = (int)capture->config.width;
    capture->height = (int)capture->config.height;
    capture->scale_factor = 1.0f;

    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:primaryDisplay
                                                    excludingWindows:@[]];

    capture->stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:capture->config
                                             delegate:nil];

    capture->output = [[MrdStreamOutput alloc] initWithCapture:capture];

    dispatch_queue_t queue = dispatch_queue_create("mrd.screencapture", DISPATCH_QUEUE_SERIAL);

    NSError *addOutputError = nil;
    if (![capture->stream addStreamOutput:capture->output
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:queue
                                   error:&addOutputError]) {
      if (error) {
        *error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Failed to add stream output: %s",
                              addOutputError.localizedDescription.UTF8String);
      }
      return FALSE;
    }

    if (@available(macOS 13.0, *)) {
      NSError *addAudioErr = nil;
      dispatch_queue_t audio_queue =
        dispatch_queue_create ("mrd.screencapture.audio", DISPATCH_QUEUE_SERIAL);
      if (![capture->stream addStreamOutput:capture->output
                                      type:SCStreamOutputTypeAudio
                        sampleHandlerQueue:audio_queue
                                     error:&addAudioErr]) {
        /* Non-fatal — audio just stays silent. */
        g_warning ("SCK audio output register failed: %s",
                   addAudioErr.localizedDescription.UTF8String);
      }
    }

    [capture->stream startCaptureWithCompletionHandler:^(NSError *err) {
      if (err) {
        g_warning ("Failed to start capture: %s", err.localizedDescription.UTF8String);
        success = FALSE;
      } else {
        success = TRUE;
      }
      dispatch_semaphore_signal(semaphore);
    }];

    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (!success) {
      if (error && !*error) {
        *error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Failed to start screen capture");
      }
      return FALSE;
    }

    capture->is_running = TRUE;
    g_message ("Screen capture started: %dx%d", capture->width, capture->height);
  }

  return TRUE;
}

void
mrd_screen_capture_stop (MrdScreenCapture *capture)
{
  g_return_if_fail (MRD_IS_SCREEN_CAPTURE (capture));

  if (!capture->is_running)
    return;

  @autoreleasepool {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [capture->stream stopCaptureWithCompletionHandler:^(NSError *err) {
      if (err) {
        g_warning ("Error stopping capture: %s", err.localizedDescription.UTF8String);
      }
      dispatch_semaphore_signal(semaphore);
    }];

    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    capture->stream = nil;
    capture->config = nil;
    capture->output = nil;
  }

  capture->is_running = FALSE;
  g_message ("Screen capture stopped");
}

void
mrd_screen_capture_set_frame_callback (MrdScreenCapture              *capture,
                                       MrdScreenCaptureFrameCallback  callback,
                                       void                          *user_data)
{
  g_return_if_fail (MRD_IS_SCREEN_CAPTURE (capture));

  capture->frame_callback = callback;
  capture->callback_user_data = user_data;
}

void
mrd_screen_capture_set_audio_callback (MrdScreenCapture              *capture,
                                       MrdScreenCaptureAudioCallback  callback,
                                       void                          *user_data)
{
  g_return_if_fail (MRD_IS_SCREEN_CAPTURE (capture));

  g_mutex_lock (&capture->frame_lock);
  capture->audio_callback = callback;
  capture->audio_callback_user_data = user_data;
  g_mutex_unlock (&capture->frame_lock);
}

void
mrd_screen_capture_get_dimensions (MrdScreenCapture *capture,
                                   int              *width,
                                   int              *height,
                                   float            *scale_factor)
{
  g_return_if_fail (MRD_IS_SCREEN_CAPTURE (capture));

  if (width)
    *width = capture->width;
  if (height)
    *height = capture->height;
  if (scale_factor)
    *scale_factor = capture->scale_factor;
}

uint8_t *
mrd_screen_capture_get_frame (MrdScreenCapture *capture,
                              uint32_t         *out_width,
                              uint32_t         *out_height,
                              uint32_t         *out_stride)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), NULL);

  g_mutex_lock (&capture->frame_lock);

  /* Always return last frame — SCK only delivers on change but each tick
   * needs something to send. */
  if (!capture->frame_buffer) {
    g_mutex_unlock (&capture->frame_lock);
    return NULL;
  }

  size_t frame_size = (size_t)capture->frame_height * capture->frame_stride;
  uint8_t *copy = g_memdup2 (capture->frame_buffer, frame_size);

  if (out_width)
    *out_width = capture->frame_width;
  if (out_height)
    *out_height = capture->frame_height;
  if (out_stride)
    *out_stride = capture->frame_stride;

  g_mutex_unlock (&capture->frame_lock);

  return copy;
}

gboolean
mrd_screen_capture_enable_nv12 (MrdScreenCapture  *capture,
                                GError           **error)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), FALSE);

  if (!capture->is_running) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Capture not running");
    return FALSE;
  }

  if (capture->nv12_mode)
    return TRUE;

  @autoreleasepool {
    g_mutex_lock (&capture->frame_lock);
    capture->nv12_mode = TRUE;
    g_clear_pointer (&capture->frame_buffer, g_free);
    capture->frame_stride = 0;
    capture->frame_ready = FALSE;
    g_mutex_unlock (&capture->frame_lock);

    capture->config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

    __block NSError *upErr = nil;
    __block gboolean ok = FALSE;
    dispatch_semaphore_t sem = dispatch_semaphore_create (0);
    [capture->stream updateConfiguration:capture->config
                       completionHandler:^(NSError *err) {
      if (err) upErr = err;
      else     ok = TRUE;
      dispatch_semaphore_signal (sem);
    }];
    dispatch_semaphore_wait (sem, DISPATCH_TIME_FOREVER);

    if (!ok) {
      g_mutex_lock (&capture->frame_lock);
      capture->nv12_mode = FALSE;
      g_mutex_unlock (&capture->frame_lock);
      if (error) {
        *error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                              "updateConfiguration to NV12 failed: %s",
                              upErr.localizedDescription.UTF8String);
      }
      return FALSE;
    }
  }

  g_message ("Screen capture: switched to NV12 (zero-copy GPU path)");
  return TRUE;
}

MrdCapturedFrame *
mrd_screen_capture_take_captured_frame (MrdScreenCapture *capture)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), NULL);

  g_mutex_lock (&capture->frame_lock);
  MrdCapturedFrame *cf = capture->latest_captured_frame;
  capture->latest_captured_frame = NULL;
  /* Reset under lock to avoid race with callback. */
  if (capture->frame_available_event)
    ResetEvent (capture->frame_available_event);
  g_mutex_unlock (&capture->frame_lock);

  return cf;
}

HANDLE
mrd_screen_capture_get_frame_event_handle (MrdScreenCapture *capture)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), NULL);
  return capture->frame_available_event;
}

static void
mrd_screen_capture_finalize (GObject *object)
{
  MrdScreenCapture *capture = MRD_SCREEN_CAPTURE (object);

  mrd_screen_capture_stop (capture);

  release_captured_frame_locked (capture->latest_captured_frame);
  capture->latest_captured_frame = NULL;

  if (capture->frame_available_event) {
    CloseHandle (capture->frame_available_event);
    capture->frame_available_event = NULL;
  }

  g_mutex_clear (&capture->frame_lock);
  g_free (capture->frame_buffer);
  g_free (capture->audio_stage);

  G_OBJECT_CLASS (mrd_screen_capture_parent_class)->finalize (object);
}

static void
mrd_screen_capture_init (MrdScreenCapture *capture)
{
  g_mutex_init (&capture->frame_lock);
  capture->frame_available_event = CreateEvent (NULL, TRUE, FALSE, NULL);
}

static void
mrd_screen_capture_class_init (MrdScreenCaptureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mrd_screen_capture_finalize;
}
