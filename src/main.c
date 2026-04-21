#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <freerdp/channels/channels.h>
#include <winpr/wtsapi.h>
#include "mrd-types.h"
#include "rdp/mrd-rdp-server.h"
static GMainLoop *main_loop = NULL;
static MrdRdpServer *rdp_server = NULL;
static char *generated_cert_file = NULL;
static char *generated_key_file = NULL;
int g_hidpi = 0;
static gboolean
on_signal(gpointer user_data) {
  g_message("Received signal, shutting down...");
  if (main_loop)
    g_main_loop_quit(main_loop);
  return G_SOURCE_REMOVE;
}
static gboolean
generate_self_signed_cert(char **out_cert_file,
                          char **out_key_file,
                          GError **error) {
  g_autofree char *cert_path = NULL;
  g_autofree char *key_path = NULL;
  g_autofree char *config_dir = NULL;
  int ret;
  config_dir = g_build_filename(g_get_home_dir(), ".config", "macos-rdp-server", NULL);
  if (g_mkdir_with_parents(config_dir, 0700) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to create config directory: %s", config_dir);
    return FALSE;
  }
  cert_path = g_build_filename(config_dir, "server.crt", NULL);
  key_path = g_build_filename(config_dir, "server.key", NULL);
  if (g_file_test(cert_path, G_FILE_TEST_EXISTS) &&
      g_file_test(key_path, G_FILE_TEST_EXISTS)) {
    g_message("Using existing certificates from %s", config_dir);
    *out_cert_file = g_steal_pointer(&cert_path);
    *out_key_file = g_steal_pointer(&key_path);
    return TRUE;
  }
  g_message("Generating self-signed certificate...");
  g_autofree char *cmd = g_strdup_printf(
      "openssl req -x509 -newkey rsa:2048 -keyout '%s' -out '%s' "
      "-days 365 -nodes -subj '/CN=macOS RDP Server' 2>/dev/null",
      key_path, cert_path);
  ret = system(cmd);
  if (ret != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to generate certificate (openssl returned %d). "
                "Make sure openssl is installed.",
                ret);
    return FALSE;
  }
  
  chmod(key_path, 0600);
  g_message("Generated certificate at %s", cert_path);
  *out_cert_file = g_steal_pointer(&cert_path);
  *out_key_file = g_steal_pointer(&key_path);
  return TRUE;
}
int main(int argc, char *argv[]) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GOptionContext) opt_context = NULL;
  int port = 3389;
  char *cert_file = NULL;
  char *key_file = NULL;
  gboolean show_help = FALSE;
  GOptionEntry entries[] = {
      {"port", 'p', 0, G_OPTION_ARG_INT, &port, "Listen port (default: 3389)", "PORT"},
      {"cert", 'c', 0, G_OPTION_ARG_FILENAME, &cert_file, "TLS certificate", "FILE"},
      {"key", 'k', 0, G_OPTION_ARG_FILENAME, &key_file, "TLS private key", "FILE"},
      {"help", 'h', 0, G_OPTION_ARG_NONE, &show_help, "Show help", NULL},
      {NULL}};
  opt_context = g_option_context_new("- macOS RDP Server");
  g_option_context_add_main_entries(opt_context, entries, NULL);
  if (!g_option_context_parse(opt_context, &argc, &argv, &error)) {
    g_printerr("Option parsing failed: %s\n", error->message);
    return 1;
  }
  if (show_help) {
    g_print("Usage: %s [OPTIONS]\n\n", argv[0]);
    g_print("macOS RDP Server - Native Remote Desktop Protocol server\n\n");
    g_print("Options:\n");
    g_print("  -p, --port PORT    Listen port (default: 3389)\n");
    g_print("  -c, --cert FILE    TLS certificate file\n");
    g_print("  -k, --key FILE     TLS private key file\n");
    g_print("  -h, --help         Show this help\n");
    return 0;
  }
  
  setpriority(PRIO_PROCESS, 0, PRIO_MAX);
  g_message("macOS RDP Server starting on port %d", port);
  
  if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi())) {
    g_printerr("Failed to initialize WTS API\n");
    return 1;
  }
  
  if (!cert_file || !key_file) {
    if (!generate_self_signed_cert(&generated_cert_file, &generated_key_file, &error)) {
      g_printerr("Certificate error: %s\n", error->message);
      return 1;
    }
    if (!cert_file)
      cert_file = generated_cert_file;
    if (!key_file)
      key_file = generated_key_file;
  }
  
  main_loop = g_main_loop_new(NULL, FALSE);
  
  g_unix_signal_add(SIGINT, on_signal, NULL);
  g_unix_signal_add(SIGTERM, on_signal, NULL);
  
  rdp_server = mrd_rdp_server_new(port, cert_file, key_file, &error);
  if (!rdp_server) {
    g_printerr("Failed to create RDP server: %s\n", error->message);
    return 1;
  }
  if (!mrd_rdp_server_start(rdp_server, &error)) {
    g_printerr("Failed to start RDP server: %s\n", error->message);
    g_object_unref(rdp_server);
    return 1;
  }
  g_message("RDP server listening on port %d", port);
  
  g_main_loop_run(main_loop);
  
  g_message("Shutting down...");
  mrd_rdp_server_stop(rdp_server);
  g_object_unref(rdp_server);
  g_main_loop_unref(main_loop);
  g_free(cert_file);
  g_free(key_file);
  g_free(generated_cert_file);
  g_free(generated_key_file);
  return 0;
}