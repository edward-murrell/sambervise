#include "sbv-search.h"

#include <string.h>

/* Trims and lower-cases search text so every panel matches the same way. */
char *
sbv_search_fold (const char *text)
{
  if (!text) return NULL;
  char *copy = g_strstrip (g_strdup (text));
  if (!*copy) {
    g_free (copy);
    return NULL;
  }
  char *folded = g_utf8_strdown (copy, -1);
  g_free (copy);
  return folded;
}

/* Case-insensitive substring match across a fixed set of fields. Walks
 * by count so one missing field can't stop the others being checked. */
gboolean
sbv_search_matches (const char *needle, const char *const *fields,
                    gsize n_fields)
{
  if (!needle) return TRUE;
  for (gsize i = 0; i < n_fields; i++) {
    if (!fields[i] || !*fields[i]) continue;
    char    *lower = g_utf8_strdown (fields[i], -1);
    gboolean hit   = strstr (lower, needle) != NULL;
    g_free (lower);
    if (hit) return TRUE;
  }
  return FALSE;
}

gint
sbv_name_compare (const char *a, const char *b)
{
  if (!a) a = "";
  if (!b) b = "";
  char *fa = g_utf8_casefold (a, -1);
  char *fb = g_utf8_casefold (b, -1);
  gint  c  = g_utf8_collate (fa, fb);
  g_free (fa);
  g_free (fb);
  return c != 0 ? c : strcmp (a, b);
}
