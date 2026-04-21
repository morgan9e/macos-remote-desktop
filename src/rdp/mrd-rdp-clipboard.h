/* CLIPRDR text-only (CF_UNICODETEXT). */

#pragma once

#include <glib-object.h>
#include <freerdp/server/cliprdr.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_RDP_CLIPBOARD (mrd_rdp_clipboard_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpClipboard, mrd_rdp_clipboard,
                      MRD, RDP_CLIPBOARD, GObject)

MrdRdpClipboard *mrd_rdp_clipboard_new (MrdSessionRdp *session,
                                        HANDLE         vcm);

gboolean mrd_rdp_clipboard_start (MrdRdpClipboard *clipboard);
void     mrd_rdp_clipboard_stop  (MrdRdpClipboard *clipboard);

/* Polled from the session loop. */
void     mrd_rdp_clipboard_poll_host        (MrdRdpClipboard *clipboard);

G_END_DECLS
