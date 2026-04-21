#include "mrd-rdp-server.h"

#include <gio/gio.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/crypto/crypto.h>
#include <winpr/ssl.h>

#include "../mrd-rdp-private.h"
#include "../util/mrd-auth.h"
#include "mrd-session-rdp.h"

struct _MrdRdpServer
{
  GObject parent;

  int port;
  char *cert_file;
  char *key_file;
  MrdAuth *auth;  /* owned */

  freerdp_listener *listener;

  GList *sessions;
  GMutex sessions_mutex;

  GThread *listener_thread;
  gboolean is_running;
  gboolean should_stop;
};

G_DEFINE_TYPE (MrdRdpServer, mrd_rdp_server, G_TYPE_OBJECT)

static BOOL
on_peer_accepted (freerdp_listener *listener,
                  freerdp_peer     *peer)
{
  MrdRdpServer *server = (MrdRdpServer *) listener->param1;
  MrdSessionRdp *session;
  GError *error = NULL;

  g_message ("New peer connection from %s", peer->hostname ? peer->hostname : "unknown");

  session = mrd_session_rdp_new (server, peer, server->cert_file, server->key_file,
                                 server->auth, &error);
  if (!session)
    {
      g_warning ("Failed to create RDP session: %s", error->message);
      g_error_free (error);
      return FALSE;
    }

  g_mutex_lock (&server->sessions_mutex);
  server->sessions = g_list_prepend (server->sessions, session);
  g_mutex_unlock (&server->sessions_mutex);

  mrd_session_rdp_start (session);

  return TRUE;
}

static gpointer
listener_thread_func (gpointer data)
{
  MrdRdpServer *server = MRD_RDP_SERVER (data);
  freerdp_listener *listener = server->listener;
  HANDLE events[32];
  DWORD event_count;

  g_debug ("Listener thread started");

  while (!server->should_stop)
    {
      event_count = listener->GetEventHandles (listener, events, 32);

      if (event_count == 0)
        {
          g_usleep (10000);  /* 10ms */
          continue;
        }

      DWORD status = WaitForMultipleObjects (event_count, events, FALSE, 100);

      if (status == WAIT_FAILED)
        {
          if (server->should_stop)
            break;
          g_warning ("WaitForMultipleObjects failed");
          break;
        }

      if (server->should_stop)
        break;

      if (!listener->CheckFileDescriptor (listener))
        {
          g_warning ("Listener check failed");
          break;
        }
    }

  g_debug ("Listener thread ending");
  return NULL;
}

MrdRdpServer *
mrd_rdp_server_new (int          port,
                    const char  *cert_file,
                    const char  *key_file,
                    MrdAuth     *auth,
                    GError     **error)
{
  MrdRdpServer *server;

  winpr_InitializeSSL (WINPR_SSL_INIT_DEFAULT);

  server = g_object_new (MRD_TYPE_RDP_SERVER, NULL);
  server->port = port;
  server->cert_file = g_strdup (cert_file);
  server->key_file = g_strdup (key_file);
  server->auth = auth;  /* takes ownership */

  server->listener = freerdp_listener_new ();
  if (!server->listener)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create FreeRDP listener");
      g_object_unref (server);
      return NULL;
    }

  server->listener->PeerAccepted = on_peer_accepted;
  server->listener->param1 = server;

  return server;
}

gboolean
mrd_rdp_server_start (MrdRdpServer  *server,
                      GError       **error)
{
  g_return_val_if_fail (MRD_IS_RDP_SERVER (server), FALSE);
  g_return_val_if_fail (!server->is_running, FALSE);

  if (!server->listener->Open (server->listener, NULL, server->port))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open listener on port %d", server->port);
      return FALSE;
    }

  server->is_running = TRUE;
  server->should_stop = FALSE;

  server->listener_thread = g_thread_new ("rdp-listener",
                                          listener_thread_func,
                                          server);

  g_message ("RDP server started on port %d", server->port);

  return TRUE;
}

void
mrd_rdp_server_stop (MrdRdpServer *server)
{
  g_return_if_fail (MRD_IS_RDP_SERVER (server));

  if (!server->is_running)
    return;

  g_message ("Stopping RDP server...");

  server->should_stop = TRUE;

  if (server->listener_thread)
    {
      g_thread_join (server->listener_thread);
      server->listener_thread = NULL;
    }

  if (server->listener)
    {
      server->listener->Close (server->listener);
    }

  g_mutex_lock (&server->sessions_mutex);
  g_list_free_full (server->sessions, g_object_unref);
  server->sessions = NULL;
  g_mutex_unlock (&server->sessions_mutex);

  server->is_running = FALSE;
}

void
mrd_rdp_server_remove_session (MrdRdpServer  *server,
                               MrdSessionRdp *session)
{
  g_return_if_fail (MRD_IS_RDP_SERVER (server));

  g_mutex_lock (&server->sessions_mutex);
  server->sessions = g_list_remove (server->sessions, session);
  g_mutex_unlock (&server->sessions_mutex);

  g_object_unref (session);
}

static void
mrd_rdp_server_finalize (GObject *object)
{
  MrdRdpServer *server = MRD_RDP_SERVER (object);

  mrd_rdp_server_stop (server);

  g_free (server->cert_file);
  g_free (server->key_file);
  mrd_auth_free (server->auth);

  if (server->listener)
    freerdp_listener_free (server->listener);

  g_mutex_clear (&server->sessions_mutex);

  G_OBJECT_CLASS (mrd_rdp_server_parent_class)->finalize (object);
}

static void
mrd_rdp_server_init (MrdRdpServer *server)
{
  g_mutex_init (&server->sessions_mutex);
}

static void
mrd_rdp_server_class_init (MrdRdpServerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mrd_rdp_server_finalize;
}
