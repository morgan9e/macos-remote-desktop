#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MrdAuth MrdAuth;

/* Auto-provisions if file_path doesn't exist (mode 0600). */
MrdAuth *mrd_auth_load (const char *file_path, GError **error);

/* Constant-time. */
gboolean mrd_auth_verify (MrdAuth    *auth,
                          const char *username,
                          const char *password);

void mrd_auth_free (MrdAuth *auth);

G_END_DECLS
