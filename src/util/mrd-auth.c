#include "mrd-auth.h"

#include <gio/gio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct
{
  char *username;
  char *password;
} MrdAuthEntry;

struct _MrdAuth
{
  GPtrArray *entries;  /* element-type MrdAuthEntry* */
};

static void
mrd_auth_entry_free (gpointer data)
{
  MrdAuthEntry *e = data;
  if (!e)
    return;
  if (e->password)
    {
      memset (e->password, 0, strlen (e->password));
      g_free (e->password);
    }
  g_free (e->username);
  g_free (e);
}

/* RFC 4648 §5, no padding. */
static char *
base64url_encode (const uint8_t *bytes, size_t len)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  size_t out_len = (len * 8 + 5) / 6;
  char *out = g_malloc (out_len + 1);

  size_t o = 0;
  uint32_t buf = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++)
    {
      buf = (buf << 8) | bytes[i];
      bits += 8;
      while (bits >= 6)
        {
          bits -= 6;
          out[o++] = alphabet[(buf >> bits) & 0x3f];
        }
    }
  if (bits > 0)
    out[o++] = alphabet[(buf << (6 - bits)) & 0x3f];
  out[o] = '\0';
  return out;
}

static gboolean
generate_random_password (char **out_password, GError **error)
{
  uint8_t raw[16];
  if (getentropy (raw, sizeof (raw)) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "getentropy failed: %s", g_strerror (errno));
      return FALSE;
    }
  *out_password = base64url_encode (raw, sizeof (raw));
  memset (raw, 0, sizeof (raw));
  return TRUE;
}

static gboolean
auto_provision (const char *file_path, MrdAuth *auth, GError **error)
{
  g_autofree char *parent = g_path_get_dirname (file_path);
  if (g_mkdir_with_parents (parent, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create auth directory %s: %s",
                   parent, g_strerror (errno));
      return FALSE;
    }

  g_autofree char *password = NULL;
  if (!generate_random_password (&password, error))
    return FALSE;

  int fd = open (file_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create auth file %s: %s",
                   file_path, g_strerror (errno));
      return FALSE;
    }

  g_autofree char *line = g_strdup_printf ("rdp:%s\n", password);
  size_t line_len = strlen (line);
  ssize_t written = write (fd, line, line_len);
  fchmod (fd, 0600);
  close (fd);

  if (written < 0 || (size_t) written != line_len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Short write to auth file %s", file_path);
      return FALSE;
    }

  MrdAuthEntry *entry = g_new0 (MrdAuthEntry, 1);
  entry->username = g_strdup ("rdp");
  entry->password = g_strdup (password);
  g_ptr_array_add (auth->entries, entry);

  /* TTY-only by default so log aggregators don't capture the password. */
  const char *print_env = g_getenv ("MRD_PRINT_AUTH");
  gboolean print_password;
  if (print_env && print_env[0] != '\0')
    print_password = g_strcmp0 (print_env, "0") != 0;
  else
    print_password = isatty (STDOUT_FILENO) != 0;

  fprintf (stdout, "** Auth file created at %s (mode 0600)\n", file_path);
  if (print_password)
    {
      fprintf (stdout, "** Generated credential: rdp / %s\n", password);
      fprintf (stdout, "** (copy this now — it won't be shown again; "
                       "set MRD_PRINT_AUTH=0 to silence)\n");
    }
  else
    {
      fprintf (stdout, "** Credential not printed (stdout is not a TTY); "
                       "read the password from %s, "
                       "or set MRD_PRINT_AUTH=1 to surface it here.\n",
               file_path);
    }
  fflush (stdout);

  memset (line, 0, line_len);
  return TRUE;
}

static gboolean
parse_line (const char *line, char **out_user, char **out_pass, GError **error)
{
  const char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r')
    return TRUE;  /* blank/comment */

  const char *colon = strchr (p, ':');
  if (!colon || colon == p)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Malformed auth line (no ':'): %s", line);
      return FALSE;
    }

  size_t user_len = (size_t) (colon - p);
  for (size_t i = 0; i < user_len; i++)
    {
      char c = p[i];
      if (c == ' ' || c == '\t' || c == ':')
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Invalid character in username");
          return FALSE;
        }
    }

  char *user = g_strndup (p, user_len);
  char *pass = g_strdup (colon + 1);

  /* Strip only trailing \r/\n; keep everything else literal. */
  size_t pl = strlen (pass);
  while (pl > 0 && (pass[pl - 1] == '\n' || pass[pl - 1] == '\r'))
    pass[--pl] = '\0';

  *out_user = user;
  *out_pass = pass;
  return TRUE;
}

MrdAuth *
mrd_auth_load (const char *file_path, GError **error)
{
  g_return_val_if_fail (file_path != NULL, NULL);

  MrdAuth *auth = g_new0 (MrdAuth, 1);
  auth->entries = g_ptr_array_new_with_free_func (mrd_auth_entry_free);

  struct stat st;
  if (stat (file_path, &st) != 0)
    {
      if (errno == ENOENT)
        {
          if (!auto_provision (file_path, auth, error))
            {
              mrd_auth_free (auth);
              return NULL;
            }
          return auth;
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "stat(%s) failed: %s", file_path, g_strerror (errno));
      mrd_auth_free (auth);
      return NULL;
    }

  if ((st.st_mode & 077) != 0)
    g_warning ("Auth file %s is group/world-accessible (mode %04o); "
               "run: chmod 600 %s",
               file_path, (unsigned) st.st_mode & 07777, file_path);

  FILE *f = fopen (file_path, "r");
  if (!f)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to open %s: %s", file_path, g_strerror (errno));
      mrd_auth_free (auth);
      return NULL;
    }

  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  while ((n = getline (&line, &cap, f)) != -1)
    {
      char *user = NULL, *pass = NULL;
      GError *line_err = NULL;
      if (!parse_line (line, &user, &pass, &line_err))
        {
          g_propagate_error (error, line_err);
          free (line);
          fclose (f);
          mrd_auth_free (auth);
          return NULL;
        }
      if (user && pass)
        {
          MrdAuthEntry *entry = g_new0 (MrdAuthEntry, 1);
          entry->username = user;
          entry->password = pass;
          g_ptr_array_add (auth->entries, entry);
        }
      else
        {
          g_free (user);
          if (pass)
            {
              memset (pass, 0, strlen (pass));
              g_free (pass);
            }
        }
    }
  free (line);
  fclose (f);

  if (auth->entries->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Auth file %s has no valid entries", file_path);
      mrd_auth_free (auth);
      return NULL;
    }

  return auth;
}

static gboolean
constant_time_equals (const char *a, const char *b)
{
  size_t la = strlen (a);
  size_t lb = strlen (b);
  /* Always walk max(la,lb) bytes so a length mismatch doesn't leak timing. */
  size_t n = la > lb ? la : lb;
  volatile unsigned char diff = (unsigned char) (la ^ lb);
  for (size_t i = 0; i < n; i++)
    {
      unsigned char ca = i < la ? (unsigned char) a[i] : 0;
      unsigned char cb = i < lb ? (unsigned char) b[i] : 0;
      diff |= (unsigned char) (ca ^ cb);
    }
  return diff == 0;
}

gboolean
mrd_auth_verify (MrdAuth    *auth,
                 const char *username,
                 const char *password)
{
  if (!auth || !username || !password)
    return FALSE;

  gboolean found = FALSE;
  for (guint i = 0; i < auth->entries->len; i++)
    {
      MrdAuthEntry *e = g_ptr_array_index (auth->entries, i);
      /* Both compared constant-time, bitwise & (no short-circuit): per-attempt
       * time must not depend on which field mismatched. */
      gboolean user_ok = constant_time_equals (e->username, username);
      gboolean pass_ok = constant_time_equals (e->password, password);
      if (user_ok & pass_ok)
        found = TRUE;
      /* loop continues to mask which entry matched */
    }
  return found;
}

void
mrd_auth_free (MrdAuth *auth)
{
  if (!auth)
    return;
  if (auth->entries)
    g_ptr_array_free (auth->entries, TRUE);
  g_free (auth);
}
