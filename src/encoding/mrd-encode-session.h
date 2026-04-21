#pragma once
#include <glib-object.h>
#include <stdint.h>
#include "../mrd-types.h"
G_BEGIN_DECLS
#define MRD_TYPE_ENCODE_SESSION (mrd_encode_session_get_type ())
G_DECLARE_FINAL_TYPE (MrdEncodeSession, mrd_encode_session, MRD, ENCODE_SESSION, GObject)

typedef void (*MrdEncodeSessionCallback) (MrdEncodeSession *session,
                                          MrdBitstream     *main_bitstream,
                                          MrdBitstream     *aux_bitstream,  
                                          void             *user_data);
MrdEncodeSession *mrd_encode_session_new (gboolean have_avc444,
                                           gboolean have_avc420);
gboolean mrd_encode_session_start (MrdEncodeSession  *session,
                                   uint32_t           width,
                                   uint32_t           height,
                                   GError           **error);
void mrd_encode_session_stop (MrdEncodeSession *session);

gboolean mrd_encode_session_encode_frame (MrdEncodeSession  *session,
                                          const uint8_t     *pixel_data,
                                          uint32_t           width,
                                          uint32_t           height,
                                          uint32_t           stride,
                                          MrdBitstream     **out_main,
                                          MrdBitstream     **out_aux,
                                          GError           **error);

gboolean mrd_encode_session_encode_pixel_buffer (MrdEncodeSession  *session,
                                                 void              *pixel_buffer,
                                                 MrdBitstream     **out_main,
                                                 MrdBitstream     **out_aux,
                                                 GError           **error);
void mrd_encode_session_set_callback (MrdEncodeSession         *session,
                                      MrdEncodeSessionCallback  callback,
                                      void                     *user_data);

MrdRdpCodec mrd_encode_session_get_codec (MrdEncodeSession *session);
G_END_DECLS