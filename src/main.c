#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

#include <winpr/wtsapi.h>
#include <freerdp/channels/channels.h>

#include "mrd-types.h"
#include "rdp/mrd-rdp-server.h"
#include "util/mrd-auth.h"

static GMainLoop *main_loop = NULL;
static MrdRdpServer *rdp_server = NULL;
static char *generated_cert_file = NULL;
static char *generated_key_file = NULL;

/* KEY=VALUE lines at ~/.config/macos-rdp-server/env → g_setenv (no overwrite).
 * Lets the user flip MRD_* knobs without editing the LaunchAgent plist, since
 * the bundle drops shell env on launchd hand-off. Existing env (shell / plist)
 * still wins so ad-hoc overrides from the terminal work. */
static void
load_env_file (void)
{
  g_autofree char *path = g_build_filename (g_get_home_dir (),
                                            ".config", "macos-rdp-server",
                                            "env", NULL);
  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &contents, &len, NULL))
    return;  /* file absent / unreadable — silent no-op */

  guint applied = 0;
  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **p = lines; *p; p++)
    {
      char *line = g_strstrip (*p);
      if (line[0] == '\0' || line[0] == '#')
        continue;
      char *eq = strchr (line, '=');
      if (!eq)
        continue;
      *eq = '\0';
      char *key = g_strstrip (line);
      char *val = g_strstrip (eq + 1);
      /* strip a single pair of surrounding quotes, if present */
      size_t vlen = strlen (val);
      if (vlen >= 2 &&
          ((val[0] == '"' && val[vlen - 1] == '"') ||
           (val[0] == '\'' && val[vlen - 1] == '\'')))
        {
          val[vlen - 1] = '\0';
          val++;
        }
      if (key[0] != '\0')
        {
          g_setenv (key, val, FALSE /* don't override shell/plist */);
          applied++;
        }
    }
  if (applied > 0)
    g_message ("Loaded %u env var(s) from %s", applied, path);
}

static gboolean
on_signal (gpointer user_data)
{
  g_message ("Received signal, shutting down...");

  if (main_loop)
    g_main_loop_quit (main_loop);

  return G_SOURCE_REMOVE;
}

static gboolean
generate_self_signed_cert (char **out_cert_file,
                           char **out_key_file,
                           GError **error)
{
  g_autofree char *cert_path = NULL;
  g_autofree char *key_path = NULL;
  g_autofree char *config_dir = NULL;

  config_dir = g_build_filename (g_get_home_dir (), ".config", "macos-rdp-server", NULL);
  if (g_mkdir_with_parents (config_dir, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create config directory: %s", config_dir);
      return FALSE;
    }

  cert_path = g_build_filename (config_dir, "server.crt", NULL);
  key_path = g_build_filename (config_dir, "server.key", NULL);

  if (g_file_test (cert_path, G_FILE_TEST_EXISTS) &&
      g_file_test (key_path, G_FILE_TEST_EXISTS))
    {
      g_message ("Using existing certificates from %s", config_dir);
      *out_cert_file = g_steal_pointer (&cert_path);
      *out_key_file = g_steal_pointer (&key_path);
      return TRUE;
    }

  g_message ("Generating self-signed certificate...");

  /* argv spawn — no shell quoting / injection via cert/key paths. */
  const char *argv[] = {
    "openssl", "req", "-x509", "-newkey", "rsa:2048",
    "-keyout", key_path,
    "-out", cert_path,
    "-days", "365", "-nodes",
    "-subj", "/CN=macOS RDP Server",
    NULL,
  };
  int wait_status = 0;
  GError *spawn_err = NULL;
  if (!g_spawn_sync (NULL, (char **) argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, &wait_status, &spawn_err))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to spawn openssl: %s. "
                   "Make sure openssl is installed.",
                   spawn_err ? spawn_err->message : "(unknown)");
      g_clear_error (&spawn_err);
      return FALSE;
    }
  GError *exit_err = NULL;
  if (!g_spawn_check_wait_status (wait_status, &exit_err))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "openssl exited with error: %s",
                   exit_err ? exit_err->message : "(unknown)");
      g_clear_error (&exit_err);
      return FALSE;
    }

  chmod (key_path, 0600);

  g_message ("Generated certificate at %s", cert_path);

  *out_cert_file = g_steal_pointer (&cert_path);
  *out_key_file = g_steal_pointer (&key_path);

  return TRUE;
}

int
main (int argc, char *argv[])
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) opt_context = NULL;

  /* stderr is block-buffered when the LaunchAgent redirects it to a file,
   * so diagnostic messages sit in the buffer until the process exits (or
   * dies). Force line-buffered so g_message lands immediately. */
  setvbuf (stderr, NULL, _IOLBF, 0);

  load_env_file ();

  int port = 3389;
  char *cert_file = NULL;
  char *key_file = NULL;
  gboolean show_help = FALSE;

  GOptionEntry entries[] = {
    { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Listen port (default: 3389)", "PORT" },
    { "cert", 'c', 0, G_OPTION_ARG_FILENAME, &cert_file, "TLS certificate", "FILE" },
    { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_file, "TLS private key", "FILE" },
    { "help", 'h', 0, G_OPTION_ARG_NONE, &show_help, "Show help", NULL },
    { NULL }
  };

  opt_context = g_option_context_new ("- macOS RDP Server");
  g_option_context_add_main_entries (opt_context, entries, NULL);

  if (!g_option_context_parse (opt_context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      return 1;
    }

  if (show_help)
    {
      g_print ("Usage: %s [OPTIONS]\n\n", argv[0]);
      g_print ("macOS RDP Server - Native Remote Desktop Protocol server\n\n");
      g_print ("Options:\n");
      g_print ("  -p, --port PORT    Listen port (default: 3389)\n");
      g_print ("  -c, --cert FILE    TLS certificate file\n");
      g_print ("  -k, --key FILE     TLS private key file\n");
      g_print ("  -h, --help         Show this help\n");
      g_print ("\nCredentials: ~/.config/macos-rdp-server/auth "
               "(auto-generated on first run)\n");
      return 0;
    }

  /* nice=20 → VT's H.264 callbacks land on E-cores; encode latency ~3×. */
  {
    const char *eff_env = g_getenv ("MRD_USE_EFFICIENCY_CORES");
    if (eff_env && eff_env[0] != '\0' && g_strcmp0 (eff_env, "0") != 0)
      {
        setpriority (PRIO_PROCESS, 0, PRIO_MAX);
        g_message ("MRD_USE_EFFICIENCY_CORES=1 — nice=20 (E-cores preferred)");
      }
  }

  g_message ("macOS RDP Server starting on port %d", port);

  {
    const char *require_env = g_getenv ("MRD_REQUIRE_AUTH");
    gboolean require = require_env && require_env[0] != '\0' &&
                       g_strcmp0 (require_env, "0") != 0;
    if (!require)
      g_warning ("*** Auth disabled (default) — ANY credentials will be accepted. "
                 "Set MRD_REQUIRE_AUTH=1 to enforce auth. ***");
  }

  if (!WTSRegisterWtsApiFunctionTable (FreeRDP_InitWtsApi ()))
    {
      g_printerr ("Failed to initialize WTS API\n");
      return 1;
    }

  /* Keep user-supplied + generated pointers distinct: avoids double-free. */
  if (!cert_file || !key_file)
    {
      if (!generate_self_signed_cert (&generated_cert_file, &generated_key_file, &error))
        {
          g_printerr ("Certificate error: %s\n", error->message);
          return 1;
        }
    }
  const char *active_cert = cert_file ? cert_file : generated_cert_file;
  const char *active_key  = key_file  ? key_file  : generated_key_file;

  main_loop = g_main_loop_new (NULL, FALSE);

  g_unix_signal_add (SIGINT, on_signal, NULL);
  g_unix_signal_add (SIGTERM, on_signal, NULL);

  g_autofree char *auth_path = g_build_filename (
    g_get_home_dir (), ".config", "macos-rdp-server", "auth", NULL);
  MrdAuth *auth = mrd_auth_load (auth_path, &error);
  if (!auth)
    {
      g_printerr ("Failed to load auth from %s: %s\n",
                  auth_path, error->message);
      return 1;
    }

  /* server takes ownership of auth */
  rdp_server = mrd_rdp_server_new (port, active_cert, active_key, auth, &error);
  if (!rdp_server)
    {
      g_printerr ("Failed to create RDP server: %s\n", error->message);
      return 1;
    }

  if (!mrd_rdp_server_start (rdp_server, &error))
    {
      g_printerr ("Failed to start RDP server: %s\n", error->message);
      g_object_unref (rdp_server);
      return 1;
    }

  g_message ("RDP server listening on port %d", port);

  g_main_loop_run (main_loop);

  g_message ("Shutting down...");
  mrd_rdp_server_stop (rdp_server);
  g_object_unref (rdp_server);
  g_main_loop_unref (main_loop);

  g_free (cert_file);
  g_free (key_file);
  g_free (generated_cert_file);
  g_free (generated_key_file);

  return 0;
}
