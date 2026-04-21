#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean mrd_telemetry_log_enabled (void);

#define MRD_TELEMETRY_LOG(...)            \
  G_STMT_START {                          \
    if (mrd_telemetry_log_enabled ())     \
      g_message (__VA_ARGS__);            \
    else                                  \
      g_debug (__VA_ARGS__);              \
  } G_STMT_END

G_END_DECLS
