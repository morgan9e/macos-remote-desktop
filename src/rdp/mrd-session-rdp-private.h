#pragma once

#include "mrd-session-rdp.h"

#include "../mrd-rdp-private.h"
#include "mrd-rdp-graphics-pipeline.h"
#include "mrd-rdp-disp.h"
#include "mrd-rdp-clipboard.h"
#include "mrd-rdp-audio.h"
#include "../platform/mrd-cursor-capture.h"
#include "../platform/mrd-virtual-display.h"
#include "../platform/mrd-screen-capture.h"
#include "../platform/mrd-input-injector.h"
#include "../encoding/mrd-encode-session.h"
#include "../util/mrd-auth.h"

#include <freerdp/peer.h>
#include <glib-object.h>
#include <winpr/synch.h>

G_BEGIN_DECLS

#define MRD_GFX_MAX_IN_FLIGHT_DEFAULT  4

#define MRD_CURSOR_CACHE_SIZE 8
#define MRD_CURSOR_POLL_US  (500 * 1000)
#define MRD_CURSOR_POS_POLL_US  (33 * 1000)
#define MRD_CURSOR_POS_POLL_MS  33

#define MRD_CLIPBOARD_POLL_US  (200 * 1000)

#define MIN_VD_WIDTH   640
#define MIN_VD_HEIGHT  480
#define MAX_VD_WIDTH   3840
#define MAX_VD_HEIGHT  2160

#define MRD_RESIZE_MIN_INTERVAL_US  (250 * 1000)

struct _MrdSessionRdp
{
  GObject parent;

  MrdRdpServer *server;
  freerdp_peer *peer;

  char *cert_file;
  char *key_file;
  MrdAuth *auth;  /* not owned; server outlives sessions */

  GThread *session_thread;
  gboolean session_should_stop;
  HANDLE stop_event;

  MrdVirtualDisplay *virtual_display;
  MrdScreenCapture *screen_capture;
  MrdInputInjector *input_injector;

  MrdEncodeSession *encode_session;

  MrdRdpGraphicsPipeline *graphics_pipeline;
  HANDLE vcm;
  gboolean drdynvc_ready;

  MrdRdpClipboard *clipboard;
  gint64 next_clipboard_poll_us;

  MrdRdpAudio *audio;

  uint16_t surface_id;
  gboolean surface_created;

  uint16_t cursor_x;
  uint16_t cursor_y;
  gboolean cursor_initialized;
  MrdCursorInfo last_cursor;
  uint16_t cursor_cache_index;
  gint64 next_cursor_check_us;
  gint64 next_cursor_pos_check_us;
  int last_cursor_seed;   /* -1: unset or SLS unavailable */
  float cursor_scale;

  /* Decremented from rdpgfx channel thread → atomic. */
  gint frames_in_flight;

  /* DISP layouts arrive on FreeRDP DVC thread, consumed on session thread. */
  MrdRdpDisp *disp;

  uint32_t client_width;
  uint32_t client_height;
  uint32_t client_scale_percent;   /* 100 if unknown */

  guint64 gfx_frame_count;
  guint64 gfx_fps_frame_count;
  guint64 gfx_skipped_count;
  gint64  gfx_fps_last_time;

  /* DISP setter on disp callback thread, consumer on session thread. */
  GMutex   resize_mutex;
  gboolean resize_pending;
  uint32_t pending_width;
  uint32_t pending_height;
  uint32_t pending_scale;
  HANDLE   resize_event;           /* manual-reset */
  gint64   last_resize_us;         /* 250ms rate limit */
};

int mrd_gfx_max_in_flight (void);

/* Frame pump — mrd-session-rdp-frame.c */
void     mrd_session_pump_frame_gfx        (MrdSessionRdp *session,
                                            gboolean       try_acquire);
void     mrd_session_on_gfx_frame_ack      (MrdRdpGraphicsPipeline *pipeline,
                                            uint32_t                frame_id,
                                            void                   *user_data);
gboolean mrd_session_on_gfx_bitrate_change (MrdRdpGraphicsPipeline *pipeline,
                                            int                     mbps,
                                            void                   *user_data);

/* Cursor — mrd-session-rdp-cursor.c */
void mrd_session_update_cursor_if_changed    (MrdSessionRdp *session);
void mrd_session_poll_server_cursor_position (MrdSessionRdp *session);

/* Handshake/input — mrd-session-rdp-handshake.c */
BOOL mrd_session_on_peer_capabilities     (freerdp_peer *peer);
BOOL mrd_session_on_peer_post_connect     (freerdp_peer *peer);
BOOL mrd_session_on_peer_activate         (freerdp_peer *peer);
BOOL mrd_session_on_keyboard_event        (rdpInput     *input,
                                           UINT16        flags,
                                           UINT8         code);
BOOL mrd_session_on_mouse_event           (rdpInput     *input,
                                           UINT16        flags,
                                           UINT16        x,
                                           UINT16        y);
BOOL mrd_session_on_extended_mouse_event  (rdpInput     *input,
                                           UINT16        flags,
                                           UINT16        x,
                                           UINT16        y);
MrdVirtualDisplay *mrd_session_create_vd_for (uint32_t   client_w,
                                              uint32_t   client_h,
                                              uint32_t   actual_scale,
                                              gboolean   use_hidpi,
                                              GError   **error);
void mrd_session_apply_vd_placement (MrdVirtualDisplay *vd);
void mrd_session_compute_scale_mode (uint32_t  raw_scale,
                                     uint32_t *out_actual_scale,
                                     gboolean *out_use_hidpi);

G_END_DECLS
