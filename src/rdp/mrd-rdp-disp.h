#pragma once

#include <glib-object.h>
#include <winpr/wtsapi.h>

#include "../mrd-types.h"

G_BEGIN_DECLS

#define MRD_TYPE_RDP_DISP (mrd_rdp_disp_get_type ())
G_DECLARE_FINAL_TYPE (MrdRdpDisp, mrd_rdp_disp, MRD, RDP_DISP, GObject)

MrdRdpDisp *mrd_rdp_disp_new (MrdSessionRdp *session_rdp,
                              HANDLE         vcm);

gboolean mrd_rdp_disp_open (MrdRdpDisp *disp);

void mrd_rdp_disp_close (MrdRdpDisp *disp);

G_END_DECLS
