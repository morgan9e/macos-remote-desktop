#pragma once
#include <glib-object.h>
#include <stdint.h>
#include "../mrd-types.h"
G_BEGIN_DECLS
#define MRD_TYPE_SCREEN_CAPTURE (mrd_screen_capture_get_type ())
G_DECLARE_FINAL_TYPE (MrdScreenCapture, mrd_screen_capture, MRD, SCREEN_CAPTURE, GObject)

typedef void (*MrdScreenCaptureFrameCallback) (MrdScreenCapture *capture,
                                                void             *pixel_data,
                                                int               width,
                                                int               height,
                                                int               stride,
                                                void             *user_data);
MrdScreenCapture *mrd_screen_capture_new (void);

MrdScreenCapture *mrd_screen_capture_new_for_display (uint32_t display_id);

MrdScreenCapture *mrd_screen_capture_new_scaled (uint32_t display_id,
                                                  uint32_t output_width,
                                                  uint32_t output_height);
gboolean mrd_screen_capture_start (MrdScreenCapture  *capture,
                                   GError           **error);
void mrd_screen_capture_stop (MrdScreenCapture *capture);
void mrd_screen_capture_set_frame_callback (MrdScreenCapture              *capture,
                                            MrdScreenCaptureFrameCallback  callback,
                                            void                          *user_data);

void mrd_screen_capture_get_dimensions (MrdScreenCapture *capture,
                                        int              *width,
                                        int              *height,
                                        float            *scale_factor);

uint8_t *mrd_screen_capture_get_frame (MrdScreenCapture *capture,
                                       uint32_t         *out_width,
                                       uint32_t         *out_height,
                                       uint32_t         *out_stride);

gboolean mrd_screen_capture_enable_nv12 (MrdScreenCapture  *capture,
                                         GError           **error);

void *mrd_screen_capture_take_pixel_buffer (MrdScreenCapture *capture);
G_END_DECLS