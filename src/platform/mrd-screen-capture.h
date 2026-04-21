#pragma once

#include <glib-object.h>
#include <stddef.h>
#include <stdint.h>
#include <cairo.h>
#include <winpr/synch.h>
#include "../mrd-types.h"

G_BEGIN_DECLS

/* NV12 frame + SCK-provided dirty region in pixel coords. */
typedef struct _MrdCapturedFrame MrdCapturedFrame;
struct _MrdCapturedFrame
{
  void           *pixel_buffer;  /* retained CVPixelBufferRef */
  cairo_region_t *dirty;         /* owned; pixel coords; may be full-frame */
};

void mrd_captured_frame_free (MrdCapturedFrame *frame);

#define MRD_TYPE_SCREEN_CAPTURE (mrd_screen_capture_get_type ())
G_DECLARE_FINAL_TYPE (MrdScreenCapture, mrd_screen_capture, MRD, SCREEN_CAPTURE, GObject)

typedef void (*MrdScreenCaptureFrameCallback) (MrdScreenCapture *capture,
                                                void             *pixel_data,
                                                int               width,
                                                int               height,
                                                int               stride,
                                                void             *user_data);

/* PCM s16 interleaved stereo @ 48 kHz. Called on SCK's audio dispatch queue. */
typedef void (*MrdScreenCaptureAudioCallback) (const int16_t *frames,
                                                size_t         n_frames,
                                                void          *user_data);

MrdScreenCapture *mrd_screen_capture_new (void);

/* display_id=0 → first available. */
MrdScreenCapture *mrd_screen_capture_new_for_display (uint32_t display_id);

/* SCK rescales backing to (output_width, output_height). */
MrdScreenCapture *mrd_screen_capture_new_scaled (uint32_t display_id,
                                                  uint32_t output_width,
                                                  uint32_t output_height);

gboolean mrd_screen_capture_start (MrdScreenCapture  *capture,
                                   GError           **error);

void mrd_screen_capture_stop (MrdScreenCapture *capture);

void mrd_screen_capture_set_frame_callback (MrdScreenCapture              *capture,
                                            MrdScreenCaptureFrameCallback  callback,
                                            void                          *user_data);

/* Register / clear the audio PCM sink. Thread-safe. Pass NULL to disable. */
void mrd_screen_capture_set_audio_callback (MrdScreenCapture              *capture,
                                            MrdScreenCaptureAudioCallback  callback,
                                            void                          *user_data);

void mrd_screen_capture_get_dimensions (MrdScreenCapture *capture,
                                        int              *width,
                                        int              *height,
                                        float            *scale_factor);

/* BGRA copy; caller g_free()s. out_stride may exceed width*4 (SCK padding). */
uint8_t *mrd_screen_capture_get_frame (MrdScreenCapture *capture,
                                       uint32_t         *out_width,
                                       uint32_t         *out_height,
                                       uint32_t         *out_stride);

/* Switches to NV12 (BT.601 video range), IOSurface-backed for zero-copy
 * VT hand-off. After this, get_frame() returns NULL; use take_captured_frame. */
gboolean mrd_screen_capture_enable_nv12 (MrdScreenCapture  *capture,
                                         GError           **error);

/* Caller owns and must free with mrd_captured_frame_free(). */
MrdCapturedFrame *mrd_screen_capture_take_captured_frame (MrdScreenCapture *capture);

/* Manual-reset; reset inside take_captured_frame. */
HANDLE mrd_screen_capture_get_frame_event_handle (MrdScreenCapture *capture);

G_END_DECLS
