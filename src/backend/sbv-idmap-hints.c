#include "sbv-idmap-hints.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── SFU30 next-ID values ──────────────────────────────────────────────── */

/* Raises `*hint` to the integer value of `attr` on `entry` when that is
 * higher. Returns TRUE when the entry had the attribute. */
static gboolean
take_max (LDAP *ld, LDAPMessage *entry, const char *attr, gint64 *hint)
{
  struct berval **bv    = ldap_get_values_len (ld, entry, attr);
  gboolean        found = FALSE;
  if (bv && bv[0]) {
    char  *s = g_strndup (bv[0]->bv_val, bv[0]->bv_len);
    gint64 v = g_ascii_strtoll (s, NULL, 10);
    g_free (s);
    if (v > *hint) *hint = v;
    found = TRUE;
  }
  ldap_value_free_len (bv);
  return found;
}

/* Reads the ADUC next-ID values. Searches the ypservers container and
 * everything under it, and keeps the highest value found. Returns TRUE
 * when a value turned up. */
static gboolean
read_sfu30 (LDAP *ld, const char *base_dn, int timeout_s, SbvIdmapHints *hints)
{
  if (!ld || !base_dn || !*base_dn) return FALSE;

  char *yp_dn = g_strdup_printf (
    "CN=ypservers,CN=ypServ30,CN=RpcServices,CN=System,%s", base_dn);
  char          *attrs[] = { (char *) "msSFU30MaxUidNumber",
                             (char *) "msSFU30MaxGidNumber", NULL };
  LDAPMessage   *result  = NULL;
  struct timeval tv      = { timeout_s, 0 };
  int rc = ldap_search_ext_s (ld, yp_dn, LDAP_SCOPE_SUBTREE,
                              "(|(msSFU30MaxUidNumber=*)(msSFU30MaxGidNumber=*))",
                              attrs, 0, NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  g_free (yp_dn);

  /* NO_SUCH_OBJECT is normal. Samba only makes the container with
   * --use-rfc2307, and only ADUC fills in the values. */
  gboolean found = FALSE;
  if (rc == LDAP_SUCCESS && result) {
    for (LDAPMessage *e = ldap_first_entry (ld, result); e;
         e = ldap_next_entry (ld, e)) {
      found |= take_max (ld, e, "msSFU30MaxUidNumber", &hints->next_uid_hint);
      found |= take_max (ld, e, "msSFU30MaxGidNumber", &hints->next_gid_hint);
    }
  }
  if (result) ldap_msgfree (result);
  return found;
}

/* Reads and parses hostadmin's record, and applies its range where the
 * profile set none. */
static void
read_record (LDAP *ld, const char *base_dn, int timeout_s, SbvIdmapHints *hints)
{
  char *dn = g_strdup_printf ("CN=%s,CN=System,%s", SBV_IDMAP_RECORD_RDN, base_dn);
  char          *attrs[] = { (char *) "keywords", NULL };
  LDAPMessage   *result  = NULL;
  struct timeval tv      = { timeout_s, 0 };
  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)", attrs, 0,
                              NULL, NULL, &tv, 1, &result);
  g_free (dn);

  LDAPMessage *entry = (rc == LDAP_SUCCESS && result) ? ldap_first_entry (ld, result) : NULL;
  if (rc == LDAP_NO_SUCH_OBJECT || (rc == LDAP_SUCCESS && !entry)) {
    hints->record_state = SBV_IDMAP_RECORD_NONE;
  } else if (rc != LDAP_SUCCESS) {
    hints->record_state = SBV_IDMAP_RECORD_UNREADABLE;
    hints->record_error = g_strdup_printf ("Couldn't read it: %s", ldap_err2string (rc));
  } else {
    struct berval **bv  = ldap_get_values_len (ld, entry, "keywords");
    int             n   = bv ? ldap_count_values_len (bv) : 0;
    char          **kw  = g_new0 (char *, n + 1);
    for (int i = 0; i < n; i++)
      kw[i] = g_strndup (bv[i]->bv_val, bv[i]->bv_len);
    ldap_value_free_len (bv);

    char           *err = NULL;
    SbvIdmapRecord *rec = sbv_idmap_record_parse ((const char * const *) kw, &err);
    g_strfreev (kw);
    if (rec) {
      hints->record_state = SBV_IDMAP_RECORD_VALID;
      hints->record       = rec;
      if (!hints->uid_range_set) {
        hints->uid_min = rec->low;
        hints->uid_max = rec->high;
      }
      if (!hints->gid_range_set) {
        hints->gid_min = rec->low;
        hints->gid_max = rec->high;
      }
    } else {
      hints->record_state = SBV_IDMAP_RECORD_INVALID;
      hints->record_error = err;
    }
  }
  if (result) ldap_msgfree (result);
}

/* Rewrites `hints->source` to say where the ranges and hints came from. */
static void
compose_source (SbvIdmapHints *hints, gboolean sfu30)
{
  gboolean  record = hints->record_state == SBV_IDMAP_RECORD_VALID;
  GString  *s      = g_string_new (NULL);
  if (hints->uid_range_set || hints->gid_range_set)
    g_string_append (s, "profile config");
  if (record && !(hints->uid_range_set && hints->gid_range_set))
    g_string_append_printf (s, "%shostadmin record (%s)", s->len ? " + " : "",
                            sbv_idmap_backend_name (hints->record->backend));
  if (s->len == 0)
    g_string_append (s, "defaults");
  if (hints->record_state == SBV_IDMAP_RECORD_INVALID)
    g_string_append (s, " (hostadmin record invalid)");
  if (sfu30)
    g_string_append (s, " + msSFU30 next IDs");
  g_free (hints->source);
  hints->source = g_string_free (s, FALSE);
}

void
sbv_idmap_hints_read_dc (LDAP *ld, const char *base_dn, int timeout_s,
                         SbvIdmapHints *hints)
{
  if (!ld || !base_dn || !*base_dn) return;
  read_record (ld, base_dn, timeout_s, hints);
  compose_source (hints, read_sfu30 (ld, base_dn, timeout_s, hints));
}

char *
sbv_idmap_hints_describe_record (const SbvIdmapHints *hints)
{
  switch (hints->record_state) {
  case SBV_IDMAP_RECORD_VALID: {
    const SbvIdmapRecord *r = hints->record;
    return g_strdup_printf ("%s, %" G_GINT64_FORMAT "–%" G_GINT64_FORMAT "%s%s",
                            sbv_idmap_backend_name (r->backend), r->low, r->high,
                            r->written_by ? ", written by " : "",
                            r->written_by ? r->written_by : "");
  }
  case SBV_IDMAP_RECORD_INVALID:
    return g_strdup_printf ("Invalid, so it's ignored. %s", hints->record_error);
  case SBV_IDMAP_RECORD_UNREADABLE:
    return g_strdup (hints->record_error);
  case SBV_IDMAP_RECORD_NONE:
  default:
    return NULL;
  }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

SbvIdmapHints *
sbv_idmap_hints_new (void)
{
  SbvIdmapHints *h = g_new0 (SbvIdmapHints, 1);
  h->uid_min = h->uid_max = -1;
  h->gid_min = h->gid_max = -1;
  h->next_uid_hint = h->next_gid_hint = -1;
  return h;
}

void
sbv_idmap_hints_free (SbvIdmapHints *self)
{
  if (!self) return;
  g_free (self->source);
  sbv_idmap_record_free (self->record);
  g_free (self->record_error);
  g_free (self);
}

SbvIdmapHints *
sbv_idmap_hints_copy (const SbvIdmapHints *self)
{
  if (!self) return NULL;
  SbvIdmapHints *c = g_memdup2 (self, sizeof *self);
  c->source       = g_strdup (self->source);
  c->record       = sbv_idmap_record_copy (self->record);
  c->record_error = g_strdup (self->record_error);
  return c;
}

SbvIdmapHints *
sbv_idmap_hints_from_profile (SbvProfile *profile)
{
  SbvIdmapHints *h = sbv_idmap_hints_new ();
  /* Profile fields default to -1 when unset; fall back to compiled-in range
   * so the UI always has something workable. */
  gint64 uid_min = sbv_profile_get_uid_min (profile);
  gint64 uid_max = sbv_profile_get_uid_max (profile);
  gint64 gid_min = sbv_profile_get_gid_min (profile);
  gint64 gid_max = sbv_profile_get_gid_max (profile);

  h->uid_min = (uid_min >= 0) ? uid_min : SBV_IDMAP_DEFAULT_UID_MIN;
  h->uid_max = (uid_max >= 0) ? uid_max : SBV_IDMAP_DEFAULT_UID_MAX;
  h->gid_min = (gid_min >= 0) ? gid_min : SBV_IDMAP_DEFAULT_GID_MIN;
  h->gid_max = (gid_max >= 0) ? gid_max : SBV_IDMAP_DEFAULT_GID_MAX;
  h->uid_range_set = uid_min >= 0 || uid_max >= 0;
  h->gid_range_set = gid_min >= 0 || gid_max >= 0;
  h->source  = g_strdup (h->uid_range_set || h->gid_range_set
                         ? "profile config" : "defaults");
  return h;
}

/* ── Async query ───────────────────────────────────────────────────────── */

typedef struct {
  SbvIdmapHints *base;   /* pre-populated from profile, owned          */
} QueryData;

static void
query_data_free (QueryData *d)
{
  /* base is consumed by g_task_return_pointer on success; freed here on
   * the error path before we hand off ownership. */
  if (d->base) sbv_idmap_hints_free (d->base);
  g_free (d);
}

static void
query_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  QueryData     *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  sbv_idmap_hints_read_dc (ld, sbv_connection_get_base_dn (conn), 10, d->base);
  sbv_connection_release_ldap (conn);

  /* Hand ownership of the hints to the task; clear our pointer so the
   * QueryData destructor doesn't double-free. */
  SbvIdmapHints *out = d->base;
  d->base = NULL;
  g_task_return_pointer (task, out, (GDestroyNotify) sbv_idmap_hints_free);
}

void
sbv_idmap_hints_query_async (SbvConnection       *conn,
                              SbvProfile          *profile,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  QueryData *d = g_new0 (QueryData, 1);
  d->base = sbv_idmap_hints_from_profile (profile);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) query_data_free);
  g_task_run_in_thread (task, query_thread);
  g_object_unref (task);
}

SbvIdmapHints *
sbv_idmap_hints_query_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* Appends "UID 11104" style text, or says the ID falls outside the range. */
static void
append_id (GString *s, const SbvIdmapRecord *r, const char *label, gint64 rid)
{
  gint64 id = sbv_idmap_record_rid_to_id (r, rid);
  if (rid < 0)
    g_string_append_printf (s, "an unknown %s", label);
  else if (id < 0)
    g_string_append_printf (s, "no %s, as its RID %" G_GINT64_FORMAT
                            " is outside the range", label, rid);
  else
    g_string_append_printf (s, "%s %" G_GINT64_FORMAT, label, id);
}

char *
sbv_idmap_hints_rid_note (const SbvIdmapHints *hints, gboolean is_user,
                          gint64 rid, gint64 primary_rid)
{
  if (!hints || hints->record_state != SBV_IDMAP_RECORD_VALID
      || hints->record->backend != SBV_IDMAP_BACKEND_RID)
    return NULL;

  const SbvIdmapRecord *r = hints->record;
  GString              *s = g_string_new (NULL);
  g_string_append_printf (s, "hostadmin's record says this domain's hosts work out "
                          "IDs from SIDs (rid), so they ignore %s here. ",
                          is_user ? "these numbers" : "this number");
  g_string_append (s, is_user ? "Hosts give this user " : "Hosts give this group ");
  append_id (s, r, is_user ? "UID" : "GID", rid);
  if (is_user) {
    g_string_append (s, " and ");
    append_id (s, r, "primary GID", primary_rid);
  }
  g_string_append_c (s, '.');
  return g_string_free (s, FALSE);
}
