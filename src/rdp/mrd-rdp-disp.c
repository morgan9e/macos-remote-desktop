/*
 * Callbacks fire on FreeRDP's internal disp thread; layout PDUs are
 * forwarded to the session thread via mrd_session_rdp_request_resize().
 */

#include "mrd-rdp-disp.h"

#include <freerdp/server/disp.h>
#include <freerdp/channels/disp.h>

#include "mrd-session-rdp.h"

#define MRD_DISP_MAX_MONITORS 1

struct _MrdRdpDisp
{
  GObject parent;

  MrdSessionRdp *session_rdp;   /* not owned */
  DispServerContext *disp_context;

  gboolean channel_opened;
  gboolean caps_sent;
};

G_DEFINE_TYPE (MrdRdpDisp, mrd_rdp_disp, G_TYPE_OBJECT)

static BOOL
disp_channel_id_assigned (DispServerContext *context,
                          uint32_t           channel_id)
{
  MrdRdpDisp *disp = context->custom;

  g_message ("[DISP] DVC channel id assigned (%u), sending caps", channel_id);

  UINT rc = context->DisplayControlCaps (context);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("[DISP] DisplayControlCaps failed: 0x%08x", rc);
      return FALSE;
    }
  disp->caps_sent = TRUE;
  return TRUE;
}

static UINT
disp_monitor_layout (DispServerContext                        *context,
                     const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *pdu)
{
  MrdRdpDisp *disp = context->custom;

  if (!disp->caps_sent)
    {
      /* Protocol violation: layout before caps. Return OK; FreeRDP tears
       * the channel down on the next round-trip. */
      g_warning ("[DISP] Protocol violation: MonitorLayout received before "
                 "caps were sent — ignoring");
      return CHANNEL_RC_OK;
    }

  if (pdu->NumMonitors == 0)
    {
      g_warning ("[DISP] Empty layout PDU — ignoring");
      return CHANNEL_RC_OK;
    }

  if (pdu->NumMonitors > MRD_DISP_MAX_MONITORS)
    {
      g_warning ("[DISP] Layout has %u monitors, max is %u — ignoring",
                 pdu->NumMonitors, MRD_DISP_MAX_MONITORS);
      return CHANNEL_RC_OK;
    }

  const DISPLAY_CONTROL_MONITOR_LAYOUT *mon = &pdu->Monitors[0];
  uint32_t width = mon->Width;
  uint32_t height = mon->Height;
  uint32_t scale = mon->DesktopScaleFactor ? mon->DesktopScaleFactor : 100;

  g_message ("[DISP] MonitorLayout: %ux%u scale=%u%% (desktop=%u device=%u)",
             width, height, scale, mon->DesktopScaleFactor,
             mon->DeviceScaleFactor);

  mrd_session_rdp_request_resize (disp->session_rdp, width, height, scale);
  return CHANNEL_RC_OK;
}

MrdRdpDisp *
mrd_rdp_disp_new (MrdSessionRdp *session_rdp,
                  HANDLE         vcm)
{
  g_return_val_if_fail (session_rdp != NULL, NULL);
  g_return_val_if_fail (vcm != NULL, NULL);

  MrdRdpDisp *disp = g_object_new (MRD_TYPE_RDP_DISP, NULL);
  disp->session_rdp = session_rdp;

  disp->disp_context = disp_server_context_new (vcm);
  if (!disp->disp_context)
    {
      g_warning ("[DISP] disp_server_context_new failed");
      g_object_unref (disp);
      return NULL;
    }

  disp->disp_context->custom = disp;
  disp->disp_context->MaxNumMonitors = MRD_DISP_MAX_MONITORS;
  disp->disp_context->MaxMonitorAreaFactorA = 8192;
  disp->disp_context->MaxMonitorAreaFactorB = 8192;

  disp->disp_context->ChannelIdAssigned = disp_channel_id_assigned;
  disp->disp_context->DispMonitorLayout = disp_monitor_layout;

  return disp;
}

gboolean
mrd_rdp_disp_open (MrdRdpDisp *disp)
{
  g_return_val_if_fail (MRD_IS_RDP_DISP (disp), FALSE);
  g_return_val_if_fail (disp->disp_context != NULL, FALSE);

  if (disp->channel_opened)
    return TRUE;

  UINT rc = disp->disp_context->Open (disp->disp_context);
  if (rc != CHANNEL_RC_OK)
    {
      g_warning ("[DISP] Open failed: 0x%08x", rc);
      return FALSE;
    }
  disp->channel_opened = TRUE;
  g_message ("[DISP] channel opened");
  return TRUE;
}

void
mrd_rdp_disp_close (MrdRdpDisp *disp)
{
  g_return_if_fail (MRD_IS_RDP_DISP (disp));

  if (disp->disp_context && disp->channel_opened)
    {
      disp->disp_context->Close (disp->disp_context);
      disp->channel_opened = FALSE;
    }
}

static void
mrd_rdp_disp_finalize (GObject *object)
{
  MrdRdpDisp *disp = MRD_RDP_DISP (object);

  mrd_rdp_disp_close (disp);
  g_clear_pointer (&disp->disp_context, disp_server_context_free);

  G_OBJECT_CLASS (mrd_rdp_disp_parent_class)->finalize (object);
}

static void
mrd_rdp_disp_init (MrdRdpDisp *disp)
{
  (void) disp;
}

static void
mrd_rdp_disp_class_init (MrdRdpDispClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = mrd_rdp_disp_finalize;
}
