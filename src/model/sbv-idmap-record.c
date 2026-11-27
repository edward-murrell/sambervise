#include "sbv-idmap-record.h"

#include <string.h>

#define PREFIX "hostadmin-idmap-"

/* Parses a plain decimal number: digits only, no sign or spaces. Returns
 * FALSE when `s` isn't one or doesn't fit in 63 bits. */
static gboolean
parse_decimal (const char *s, gint64 *out)
{
  if (!s || !*s) return FALSE;
  guint64 v = 0;
  for (const char *p = s; *p; p++) {
    if (!g_ascii_isdigit (*p)) return FALSE;
    if (v > (guint64) (G_MAXINT64 - 9) / 10) return FALSE;
    v = v * 10 + (guint64) (*p - '0');
  }
  *out = (gint64) v;
  return TRUE;
}

/* Parses "<low>-<high>". Returns FALSE when malformed or not rising. */
static gboolean
parse_range (const char *s, gint64 *low, gint64 *high)
{
  const char *dash = strchr (s, '-');
  if (!dash) return FALSE;
  char    *lo_text = g_strndup (s, dash - s);
  gboolean ok      = parse_decimal (lo_text, low) && parse_decimal (dash + 1, high);
  g_free (lo_text);
  return ok && *high > *low;
}

SbvIdmapRecord *
sbv_idmap_record_parse (const char * const *keywords, char **error)
{
  *error = NULL;
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (guint i = 0; keywords && keywords[i]; i++) {
    const char *kw = keywords[i];
    if (!g_str_has_prefix (kw, PREFIX)) continue;
    const char *eq = strchr (kw + strlen (PREFIX), '=');
    if (!eq) continue;
    char *name = g_strndup (kw + strlen (PREFIX), eq - kw - strlen (PREFIX));
    if (g_hash_table_contains (seen, name)) {
      *error = g_strdup_printf ("The setting \"%s\" appears more than once.", name);
      g_free (name);
      g_hash_table_unref (seen);
      return NULL;
    }
    g_hash_table_insert (seen, name, g_strdup (eq + 1));
  }

  SbvIdmapRecord *r = g_new0 (SbvIdmapRecord, 1);
  r->low  = SBV_IDMAP_RECORD_LOW;
  r->high = SBV_IDMAP_RECORD_HIGH;

  const char *version = g_hash_table_lookup (seen, "version");
  const char *backend = g_hash_table_lookup (seen, "backend");
  const char *range   = g_hash_table_lookup (seen, "range");

  if (!version || !parse_decimal (version, &r->version) || r->version < 1)
    *error = g_strdup ("The version is missing or isn't a whole number.");
  else if (r->version > SBV_IDMAP_RECORD_VERSION)
    *error = g_strdup_printf ("The record is format version %" G_GINT64_FORMAT
                              ", and Sambervise only knows version %d.",
                              r->version, SBV_IDMAP_RECORD_VERSION);
  else if (g_strcmp0 (backend, "rid") == 0)
    r->backend = SBV_IDMAP_BACKEND_RID;
  else if (g_strcmp0 (backend, "ad") == 0)
    r->backend = SBV_IDMAP_BACKEND_AD;
  else
    *error = backend
      ? g_strdup_printf ("The backend \"%s\" isn't rid or ad.", backend)
      : g_strdup ("The backend is missing.");

  if (!*error && range && !parse_range (range, &r->low, &r->high))
    *error = g_strdup_printf ("The range \"%s\" isn't two rising numbers, "
                              "such as 10000-999999.", range);

  if (!*error) {
    r->source     = g_strdup (g_hash_table_lookup (seen, "source"));
    r->written_by = g_strdup (g_hash_table_lookup (seen, "written-by"));
    r->written    = g_strdup (g_hash_table_lookup (seen, "written"));
  }
  g_hash_table_unref (seen);

  if (*error) {
    sbv_idmap_record_free (r);
    return NULL;
  }
  return r;
}

SbvIdmapRecord *
sbv_idmap_record_copy (const SbvIdmapRecord *record)
{
  if (!record) return NULL;
  SbvIdmapRecord *c = g_memdup2 (record, sizeof *record);
  c->source     = g_strdup (record->source);
  c->written_by = g_strdup (record->written_by);
  c->written    = g_strdup (record->written);
  return c;
}

void
sbv_idmap_record_free (SbvIdmapRecord *record)
{
  if (!record) return;
  g_free (record->source);
  g_free (record->written_by);
  g_free (record->written);
  g_free (record);
}

gint64
sbv_idmap_record_rid_to_id (const SbvIdmapRecord *record, gint64 rid)
{
  if (!record || rid < 0) return -1;
  gint64 id = record->low + rid;
  return id <= record->high ? id : -1;
}

gint64
sbv_sid_rid (const guchar *sid, gsize len)
{
  if (!sid || len < 8) return -1;
  guchar count = sid[1];
  if (count == 0 || len < 8 + (gsize) count * 4) return -1;
  const guchar *p = sid + 8 + (count - 1) * 4;
  return (gint64) ((guint32) p[0] | ((guint32) p[1] << 8)
                   | ((guint32) p[2] << 16) | ((guint32) p[3] << 24));
}

const char *
sbv_idmap_backend_name (SbvIdmapBackend backend)
{
  return backend == SBV_IDMAP_BACKEND_RID ? "rid" : "ad";
}
