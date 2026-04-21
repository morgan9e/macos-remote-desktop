#pragma once

#include <glib-object.h>
#include <cairo.h>
#include <stdint.h>
#include <winpr/synch.h>
#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_ENCODE_SESSION (mrd_encode_session_get_type ())
G_DECLARE_FINAL_TYPE (MrdEncodeSession, mrd_encode_session, MRD, ENCODE_SESSION, GObject)

typedef void (*MrdEncodeSessionCallback) (MrdEncodeSession *session,
                                          MrdBitstream     *main_bitstream,
                                          MrdBitstream     *aux_bitstream,  /* NULL for AVC420 */
                                          void             *user_data);

MrdEncodeSession *mrd_encode_session_new (gboolean have_avc444,
                                           gboolean have_avc420);

gboolean mrd_encode_session_start (MrdEncodeSession  *session,
                                   uint32_t           width,
                                   uint32_t           height,
                                   GError           **error);

void mrd_encode_session_stop (MrdEncodeSession *session);

/* BGRA pixel_data. */
gboolean mrd_encode_session_encode_frame (MrdEncodeSession  *session,
                                          const uint8_t     *pixel_data,
                                          uint32_t           width,
                                          uint32_t           height,
                                          uint32_t           stride,
                                          MrdBitstream     **out_main,
                                          MrdBitstream     **out_aux,
                                          GError           **error);

/* Synchronous (CompleteFrames(pts)); prefer submit/drain. NV12 IOSurface
 * CVPixelBufferRef cast to void*; caller retains ownership. */
gboolean mrd_encode_session_encode_pixel_buffer (MrdEncodeSession  *session,
                                                 void              *pixel_buffer,
                                                 MrdBitstream     **out_main,
                                                 MrdBitstream     **out_aux,
                                                 GError           **error);

typedef struct {
  MrdBitstream    *main_bs;
  MrdBitstream    *aux_bs;          /* NULL for AVC420 */
  cairo_region_t  *damage_region;   /* owned, may be NULL */
  int64_t          encode_us;
  size_t           payload_bytes;
} MrdEncodedFrame;

void mrd_encoded_frame_free (MrdEncodedFrame *frame);

/* Returns bitstreams to the pool — preferred over mrd_encoded_frame_free. */
void mrd_encode_session_release_frame (MrdEncodeSession *session,
                                       MrdEncodedFrame  *frame);

/* damage_region ownership transfers; pixel_buffer is retained internally. */
gboolean mrd_encode_session_submit_pixel_buffer (MrdEncodeSession  *session,
                                                 void              *pixel_buffer,
                                                 cairo_region_t    *damage_region,
                                                 GError           **error);

/* Non-blocking; appends MrdEncodedFrame* to out_frames (caller-owned). */
guint mrd_encode_session_drain_ready (MrdEncodeSession *session,
                                      GQueue           *out_frames);

/* Manual-reset; owned by session. */
HANDLE mrd_encode_session_get_output_event_handle (MrdEncodeSession *session);

/* Frames in VT + completed-but-not-drained (backpressure source). */
guint mrd_encode_session_get_outstanding (MrdEncodeSession *session);

/* mbps: 0 = uncapped, N = N Mbps avg / ~1.5N peak over 1 s. Returns
 * FALSE on VT rejection — caller should stop driving updates. */
gboolean mrd_encode_session_set_bitrate_mbps (MrdEncodeSession *session,
                                              int               mbps);

/* MRD_BITRATE_MBPS default; adaptive controller uses this as ceiling. */
int mrd_encode_session_get_initial_bitrate_mbps (void);

void mrd_encode_session_set_callback (MrdEncodeSession         *session,
                                      MrdEncodeSessionCallback  callback,
                                      void                     *user_data);

MrdRdpCodec mrd_encode_session_get_codec (MrdEncodeSession *session);

G_END_DECLS
