/*
 * MS-RDPEA rdpsnd server. Advertises PCM 48k/stereo/s16; capture thread
 * enqueues into a ring, session loop drains to SendSamples.
 */

#include "mrd-rdp-audio.h"

#include <gio/gio.h>
#include <string.h>

#include <freerdp/codec/audio.h>

#define MRD_AUDIO_RING_FRAMES    16384u
#define MRD_AUDIO_CHANNELS       2u
#define MRD_AUDIO_SAMPLE_RATE    48000u
#define MRD_AUDIO_BITS_PER_SAMPLE 16u
#define MRD_AUDIO_BLOCK_ALIGN    (MRD_AUDIO_CHANNELS * (MRD_AUDIO_BITS_PER_SAMPLE / 8))
#define MRD_AUDIO_CHUNK_FRAMES   960u   /* 20 ms @ 48 kHz */

struct _MrdRdpAudio
{
  GObject parent;

  MrdSessionRdp *session;  /* not owned */
  HANDLE vcm;              /* not owned */

  RdpsndServerContext *ctx;
  gboolean started;

  AUDIO_FORMAT server_format_pcm;

  gint activated;

  GMutex   ring_lock;
  int16_t *ring;
  uint32_t ring_head;
  uint32_t ring_tail;
};

G_DEFINE_TYPE (MrdRdpAudio, mrd_rdp_audio, G_TYPE_OBJECT)

static uint32_t
ring_used_locked (MrdRdpAudio *self)
{
  return self->ring_head - self->ring_tail;
}

static uint32_t
ring_free_locked (MrdRdpAudio *self)
{
  return MRD_AUDIO_RING_FRAMES - ring_used_locked (self);
}

static void
on_activated (RdpsndServerContext *ctx)
{
  MrdRdpAudio *self = ctx->data;

  UINT16 pick = UINT16_MAX;
  for (UINT16 i = 0; i < ctx->num_client_formats; i++)
    {
      if (audio_format_compatible (&self->server_format_pcm,
                                   &ctx->client_formats[i]))
        {
          pick = i;
          break;
        }
    }
  if (pick == UINT16_MAX)
    return;

  if (ctx->SelectFormat (ctx, pick) != CHANNEL_RC_OK)
    return;

  g_atomic_int_set (&self->activated, 1);
}

MrdRdpAudio *
mrd_rdp_audio_new (MrdSessionRdp *session, HANDLE vcm)
{
  g_return_val_if_fail (vcm != NULL, NULL);

  MrdRdpAudio *self = g_object_new (MRD_TYPE_RDP_AUDIO, NULL);
  self->session = session;
  self->vcm = vcm;

  self->ctx = rdpsnd_server_context_new (vcm);
  if (!self->ctx)
    {
      g_object_unref (self);
      return NULL;
    }

  self->server_format_pcm.wFormatTag = WAVE_FORMAT_PCM;
  self->server_format_pcm.nChannels = MRD_AUDIO_CHANNELS;
  self->server_format_pcm.nSamplesPerSec = MRD_AUDIO_SAMPLE_RATE;
  self->server_format_pcm.nBlockAlign = MRD_AUDIO_BLOCK_ALIGN;
  self->server_format_pcm.wBitsPerSample = MRD_AUDIO_BITS_PER_SAMPLE;
  self->server_format_pcm.nAvgBytesPerSec =
    MRD_AUDIO_SAMPLE_RATE * MRD_AUDIO_BLOCK_ALIGN;

  self->ctx->use_dynamic_virtual_channel = TRUE;
  self->ctx->data = self;
  self->ctx->num_server_formats = 1;
  self->ctx->server_formats = &self->server_format_pcm;
  self->ctx->src_format     = &self->server_format_pcm;
  self->ctx->latency        = 50;
  self->ctx->Activated      = on_activated;

  self->ring = g_new0 (int16_t, MRD_AUDIO_RING_FRAMES * MRD_AUDIO_CHANNELS);

  return self;
}

gboolean
mrd_rdp_audio_start (MrdRdpAudio *self)
{
  g_return_val_if_fail (MRD_IS_RDP_AUDIO (self), FALSE);
  if (self->started)
    return TRUE;

  if (self->ctx->Initialize (self->ctx, FALSE) != CHANNEL_RC_OK)
    return FALSE;

  self->started = TRUE;
  return TRUE;
}

void
mrd_rdp_audio_stop (MrdRdpAudio *self)
{
  g_return_if_fail (MRD_IS_RDP_AUDIO (self));
  if (!self->started)
    return;

  g_atomic_int_set (&self->activated, 0);
  /* Skip ctx->Close: WTSVirtualChannelWrite(SNDC_CLOSE) hangs when the peer
   * socket is already dead. rdpsnd_server_context_free handles cleanup. */
  self->started = FALSE;
}

HANDLE
mrd_rdp_audio_get_event_handle (MrdRdpAudio *self)
{
  g_return_val_if_fail (MRD_IS_RDP_AUDIO (self), NULL);
  if (!self->started || !self->ctx)
    return NULL;
  return rdpsnd_server_get_event_handle (self->ctx);
}

gboolean
mrd_rdp_audio_handle_messages (MrdRdpAudio *self)
{
  g_return_val_if_fail (MRD_IS_RDP_AUDIO (self), FALSE);
  if (!self->started || !self->ctx)
    return FALSE;

  UINT rc = rdpsnd_server_handle_messages (self->ctx);
  return rc == CHANNEL_RC_OK || rc == ERROR_NO_DATA;
}

void
mrd_rdp_audio_push_pcm (MrdRdpAudio   *self,
                        const int16_t *frames,
                        size_t         n_frames)
{
  g_return_if_fail (MRD_IS_RDP_AUDIO (self));
  if (!frames || n_frames == 0)
    return;
  if (!g_atomic_int_get (&self->activated))
    return;

  g_mutex_lock (&self->ring_lock);

  uint32_t avail = ring_free_locked (self);
  if ((uint32_t) n_frames > avail)
    {
      uint32_t drop = (uint32_t) n_frames - avail;
      if (drop > MRD_AUDIO_RING_FRAMES)
        drop = MRD_AUDIO_RING_FRAMES;
      self->ring_tail += drop;
    }

  uint32_t head_idx = self->ring_head & (MRD_AUDIO_RING_FRAMES - 1);
  uint32_t first = MIN ((uint32_t) n_frames, MRD_AUDIO_RING_FRAMES - head_idx);
  memcpy (&self->ring[head_idx * MRD_AUDIO_CHANNELS],
          frames,
          (size_t) first * MRD_AUDIO_CHANNELS * sizeof (int16_t));
  uint32_t remain = (uint32_t) n_frames - first;
  if (remain)
    memcpy (&self->ring[0],
            frames + (size_t) first * MRD_AUDIO_CHANNELS,
            (size_t) remain * MRD_AUDIO_CHANNELS * sizeof (int16_t));
  self->ring_head += (uint32_t) n_frames;

  g_mutex_unlock (&self->ring_lock);
}

static uint32_t
ring_drain (MrdRdpAudio *self, int16_t *out, uint32_t want_frames)
{
  g_mutex_lock (&self->ring_lock);
  uint32_t used = ring_used_locked (self);
  uint32_t take = MIN (want_frames, used);
  if (take == 0)
    {
      g_mutex_unlock (&self->ring_lock);
      return 0;
    }
  uint32_t tail_idx = self->ring_tail & (MRD_AUDIO_RING_FRAMES - 1);
  uint32_t first = MIN (take, MRD_AUDIO_RING_FRAMES - tail_idx);
  memcpy (out,
          &self->ring[tail_idx * MRD_AUDIO_CHANNELS],
          (size_t) first * MRD_AUDIO_CHANNELS * sizeof (int16_t));
  uint32_t remain = take - first;
  if (remain)
    memcpy (out + (size_t) first * MRD_AUDIO_CHANNELS,
            &self->ring[0],
            (size_t) remain * MRD_AUDIO_CHANNELS * sizeof (int16_t));
  self->ring_tail += take;
  g_mutex_unlock (&self->ring_lock);
  return take;
}

void
mrd_rdp_audio_pump (MrdRdpAudio *self)
{
  g_return_if_fail (MRD_IS_RDP_AUDIO (self));
  if (!self->started || !self->ctx)
    return;
  if (!g_atomic_int_get (&self->activated))
    return;

  int16_t chunk[MRD_AUDIO_CHUNK_FRAMES * MRD_AUDIO_CHANNELS];
  for (int i = 0; i < 4; i++)
    {
      uint32_t got = ring_drain (self, chunk, MRD_AUDIO_CHUNK_FRAMES);
      if (got == 0)
        break;
      if (self->ctx->SendSamples (self->ctx, chunk, got, 0) != CHANNEL_RC_OK)
        {
          g_atomic_int_set (&self->activated, 0);
          return;
        }
    }
}

static void
mrd_rdp_audio_finalize (GObject *object)
{
  MrdRdpAudio *self = MRD_RDP_AUDIO (object);

  if (self->started)
    mrd_rdp_audio_stop (self);
  if (self->ctx)
    {
      /* server_formats points at self->server_format_pcm (stack-of-struct),
       * not a heap allocation. rdpsnd_server_context_free blindly free()s
       * it, which aborts. NULL it out first. */
      self->ctx->server_formats = NULL;
      self->ctx->num_server_formats = 0;
      rdpsnd_server_context_free (self->ctx);
      self->ctx = NULL;
    }
  g_free (self->ring);
  self->ring = NULL;
  g_mutex_clear (&self->ring_lock);

  G_OBJECT_CLASS (mrd_rdp_audio_parent_class)->finalize (object);
}

static void
mrd_rdp_audio_init (MrdRdpAudio *self)
{
  g_mutex_init (&self->ring_lock);
}

static void
mrd_rdp_audio_class_init (MrdRdpAudioClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_rdp_audio_finalize;
}
