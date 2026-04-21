/* MS-RDPEA rdpsnd server: PCM (48 kHz / stereo / s16) over DVC. */

#pragma once

#include <glib-object.h>
#include <stdint.h>
#include <freerdp/server/rdpsnd.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_RDP_AUDIO (mrd_rdp_audio_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpAudio, mrd_rdp_audio, MRD, RDP_AUDIO, GObject)

MrdRdpAudio *mrd_rdp_audio_new   (MrdSessionRdp *session, HANDLE vcm);

/* Opens the DVC and starts listening for client formats. */
gboolean     mrd_rdp_audio_start (MrdRdpAudio *audio);
void         mrd_rdp_audio_stop  (MrdRdpAudio *audio);

/* Session loop pumps pending server⇄client messages. */
HANDLE       mrd_rdp_audio_get_event_handle (MrdRdpAudio *audio);
gboolean     mrd_rdp_audio_handle_messages  (MrdRdpAudio *audio);

/* Session loop drains the ring and emits SendSamples / silence. */
void         mrd_rdp_audio_pump (MrdRdpAudio *audio);

/* Called from capture thread. PCM interleaved s16le at negotiated format. */
void         mrd_rdp_audio_push_pcm (MrdRdpAudio   *audio,
                                     const int16_t *frames,
                                     size_t         n_frames);

G_END_DECLS
