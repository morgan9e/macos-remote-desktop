#include "mrd-telemetry.h"

gboolean
mrd_telemetry_log_enabled (void)
{
  static gsize init = 0;
  static gboolean cached = FALSE;

  if (g_once_init_enter (&init))
    {
      const char *env = g_getenv ("MRD_TELEMETRY_LOG");
      cached = env && env[0] != '\0' && g_strcmp0 (env, "0") != 0;
      g_once_init_leave (&init, 1);
    }

  return cached;
}
