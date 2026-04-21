#pragma once

#include <glib-object.h>
#include <freerdp/listener.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

typedef struct _MrdSessionRdp MrdSessionRdp;
typedef struct _MrdAuth MrdAuth;

#define MRD_TYPE_RDP_SERVER (mrd_rdp_server_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpServer, mrd_rdp_server, MRD, RDP_SERVER, GObject)

MrdRdpServer *mrd_rdp_server_new (int          port,
                                   const char  *cert_file,
                                   const char  *key_file,
                                   MrdAuth     *auth,
                                   GError     **error);

gboolean mrd_rdp_server_start (MrdRdpServer  *server,
                                GError       **error);

void mrd_rdp_server_stop (MrdRdpServer *server);

/* Unrefs the session. */
void mrd_rdp_server_remove_session (MrdRdpServer  *server,
                                     MrdSessionRdp *session);

G_END_DECLS
