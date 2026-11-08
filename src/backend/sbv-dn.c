#include "sbv-dn.h"

#include <string.h>

/* Returns a pointer to the first comma in `dn` that is not escaped with a
 * backslash, or NULL if there is none. */
static const char *
find_unescaped_comma (const char *dn)
{
  for (const char *p = dn; *p; p++) {
    if (*p == '\\') {
      if (p[1] == '\0') return NULL;
      p++;
      continue;
    }
    if (*p == ',') return p;
  }
  return NULL;
}

/* Converts one hex digit to its value, or -1 if it isn't one. */
static int
hex_value (char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Reverses RFC 4514 escaping in an RDN value: "\," → "," and "\2C" → ",".
 * Caller frees. */
static char *
unescape_value (const char *value, gsize len)
{
  GString *out = g_string_sized_new (len);
  for (gsize i = 0; i < len; i++) {
    char c = value[i];
    if (c == '\\' && i + 1 < len) {
      int hi = hex_value (value[i + 1]);
      int lo = (i + 2 < len) ? hex_value (value[i + 2]) : -1;
      if (hi >= 0 && lo >= 0) {
        g_string_append_c (out, (char) ((hi << 4) | lo));
        i += 2;
      } else {
        g_string_append_c (out, value[i + 1]);
        i += 1;
      }
      continue;
    }
    g_string_append_c (out, c);
  }
  return g_string_free (out, FALSE);
}

/* Unescaped value of a single RDN string such as "OU=Sales". NULL if the
 * RDN has no "=". Caller frees. */
static char *
rdn_value (const char *rdn, gsize len)
{
  const char *eq = memchr (rdn, '=', len);
  if (!eq) return NULL;
  gsize off = (gsize) (eq - rdn) + 1;
  return unescape_value (eq + 1, len - off);
}

char *
sbv_dn_escape_value (const char *value)
{
  if (!value) return g_strdup ("");

  gsize    len = strlen (value);
  GString *out = g_string_sized_new (len + 8);

  for (gsize i = 0; i < len; i++) {
    char c = value[i];
    gboolean special = strchr (",+\"\\<>;=", c) != NULL;
    gboolean lead    = (i == 0)       && (c == ' ' || c == '#');
    gboolean trail   = (i == len - 1) && c == ' ';
    if (special || lead || trail)
      g_string_append_c (out, '\\');
    g_string_append_c (out, c);
  }
  return g_string_free (out, FALSE);
}

gboolean
sbv_dn_split (const char *dn, char **rdn, char **parent)
{
  if (rdn)    *rdn    = NULL;
  if (parent) *parent = NULL;
  if (!dn) return FALSE;

  const char *comma = find_unescaped_comma (dn);
  if (!comma) return FALSE;

  const char *rest = comma + 1;
  while (*rest == ' ') rest++;
  if (!*rest) return FALSE;

  if (rdn)    *rdn    = g_strndup (dn, (gsize) (comma - dn));
  if (parent) *parent = g_strdup (rest);
  return TRUE;
}

char *
sbv_dn_rdn_value (const char *dn)
{
  if (!dn) return NULL;
  const char *comma = find_unescaped_comma (dn);
  gsize len = comma ? (gsize) (comma - dn) : strlen (dn);
  return rdn_value (dn, len);
}

gboolean
sbv_dn_equal (const char *a, const char *b)
{
  if (!a || !b) return a == b;
  return g_ascii_strcasecmp (a, b) == 0;
}

gboolean
sbv_dn_is_within (const char *dn, const char *ancestor)
{
  if (!dn || !ancestor || !*ancestor) return FALSE;

  gsize dl = strlen (dn);
  gsize al = strlen (ancestor);
  if (dl < al) return FALSE;
  if (g_ascii_strcasecmp (dn + dl - al, ancestor) != 0) return FALSE;
  if (dl == al) return TRUE;

  /* The suffix must start right after an unescaped comma, so that
   * "OU=XSales,..." isn't treated as inside "OU=Sales,...". */
  const char *sep = dn + dl - al - 1;
  if (*sep != ',') return FALSE;
  gsize backslashes = 0;
  for (const char *p = sep - 1; p >= dn && *p == '\\'; p--)
    backslashes++;
  return (backslashes % 2) == 0;
}

char *
sbv_dn_domain_name (const char *dn)
{
  if (!dn) return NULL;

  GString    *out = g_string_new (NULL);
  const char *p   = dn;
  while (p && *p) {
    while (*p == ' ') p++;
    const char *comma = find_unescaped_comma (p);
    gsize len = comma ? (gsize) (comma - p) : strlen (p);
    if (len < 3 || g_ascii_strncasecmp (p, "DC=", 3) != 0) {
      g_string_free (out, TRUE);
      return g_strdup (dn);
    }
    char *v = rdn_value (p, len);
    if (out->len) g_string_append_c (out, '.');
    g_string_append (out, v);
    g_free (v);
    p = comma ? comma + 1 : NULL;
  }
  return g_string_free (out, FALSE);
}

char **
sbv_dn_path_components (const char *dn, const char *base)
{
  if (!sbv_dn_is_within (dn, base)) return NULL;

  GPtrArray *parts = g_ptr_array_new ();
  gsize      stop  = strlen (dn) - strlen (base);   /* offset of base */
  const char *p    = dn;

  while ((gsize) (p - dn) < stop) {
    const char *comma = find_unescaped_comma (p);
    gsize len = comma ? (gsize) (comma - p) : strlen (p);
    char *v = rdn_value (p, len);
    g_ptr_array_add (parts, v ? v : g_strndup (p, len));
    if (!comma) break;
    p = comma + 1;
    while (*p == ' ') p++;
  }

  /* Collected bottom-up; reverse so the top-level OU comes first. */
  guint n = parts->len;
  char **out = g_new0 (char *, n + 1);
  for (guint i = 0; i < n; i++)
    out[i] = g_ptr_array_index (parts, n - 1 - i);
  g_ptr_array_free (parts, TRUE);
  return out;
}
