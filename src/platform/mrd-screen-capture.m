#include "mrd-screen-capture.h"
#include <gio/gio.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>

@interface MrdStreamOutput : NSObject <SCStreamOutput>
{
  MrdScreenCapture *_capture;
}
- (instancetype)initWithCapture:(MrdScreenCapture *)capture;
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
  
  uint32_t output_width;
  uint32_t output_height;
  gboolean scales_to_fit;
  
  GMutex frame_lock;
  uint8_t *frame_buffer;
  uint32_t frame_width;
  uint32_t frame_height;
  uint32_t frame_stride;
  gboolean frame_ready;
  
  gboolean nv12_mode;
  CVPixelBufferRef latest_pixel_buffer;  
};
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
  if (type != SCStreamOutputTypeScreen)
    return;
  CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!imageBuffer)
    return;
  
  if (_capture->nv12_mode) {
    CVPixelBufferRetain (imageBuffer);
    g_mutex_lock (&_capture->frame_lock);
    if (_capture->latest_pixel_buffer)
      CVPixelBufferRelease (_capture->latest_pixel_buffer);
    _capture->latest_pixel_buffer = imageBuffer;
    _capture->frame_width  = (uint32_t)CVPixelBufferGetWidth (imageBuffer);
    _capture->frame_height = (uint32_t)CVPixelBufferGetHeight (imageBuffer);
    _capture->frame_ready = TRUE;
    g_mutex_unlock (&_capture->frame_lock);
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
    capture->config.showsCursor = NO;  
    capture->config.queueDepth = 4;
    
    capture->config.minimumFrameInterval = CMTimeMake(1, 120);
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
void *
mrd_screen_capture_take_pixel_buffer (MrdScreenCapture *capture)
{
  g_return_val_if_fail (MRD_IS_SCREEN_CAPTURE (capture), NULL);
  g_mutex_lock (&capture->frame_lock);
  CVPixelBufferRef pb = capture->latest_pixel_buffer;
  capture->latest_pixel_buffer = NULL;
  g_mutex_unlock (&capture->frame_lock);
  return pb;  
}

static void
mrd_screen_capture_finalize (GObject *object)
{
  MrdScreenCapture *capture = MRD_SCREEN_CAPTURE (object);
  mrd_screen_capture_stop (capture);
  if (capture->latest_pixel_buffer)
    CVPixelBufferRelease (capture->latest_pixel_buffer);
  g_mutex_clear (&capture->frame_lock);
  g_free (capture->frame_buffer);
  G_OBJECT_CLASS (mrd_screen_capture_parent_class)->finalize (object);
}
static void
mrd_screen_capture_init (MrdScreenCapture *capture)
{
  g_mutex_init (&capture->frame_lock);
}
static void
mrd_screen_capture_class_init (MrdScreenCaptureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_screen_capture_finalize;
}