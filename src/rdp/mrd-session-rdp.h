#pragma once

#include <glib-object.h>
#include <stdint.h>
#include <freerdp/peer.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

typedef struct _MrdAuth MrdAuth;

#define MRD_TYPE_SESSION_RDP (mrd_session_rdp_get_type ())
G_DECLARE_FINAL_TYPE (MrdSessionRdp, mrd_session_rdp, MRD, SESSION_RDP, GObject)

MrdSessionRdp *mrd_session_rdp_new (MrdRdpServer   *server,
                                    freerdp_peer   *peer,
                                    const char     *cert_file,
                                    const char     *key_file,
                                    MrdAuth        *auth,
                                    GError        **error);

void mrd_session_rdp_start (MrdSessionRdp *session);

void mrd_session_rdp_stop (MrdSessionRdp *session);

MrdRdpGraphicsPipeline *mrd_session_rdp_get_graphics_pipeline (MrdSessionRdp *session);

/* Called from the FreeRDP DISP DVC thread; coalesces by overwriting any
 * pending tuple. scale_percent=100 if unknown. */
void mrd_session_rdp_request_resize (MrdSessionRdp *session,
                                     uint32_t       width,
                                     uint32_t       height,
                                     uint32_t       scale_percent);

G_END_DECLS
