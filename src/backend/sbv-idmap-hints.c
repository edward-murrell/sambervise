#include "sbv-idmap-hints.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

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
  g_free (self);
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
  h->source  = g_strdup ("profile config / defaults");
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

  const char *base = sbv_connection_get_base_dn (conn);
  /* Well-known SFU30 location for max-assigned UID/GID hints. */
  char *yp_dn = g_strdup_printf (
    "CN=ypservers,CN=ypServ30,CN=RpcServices,CN=System,%s", base);

  const char *attrs[] = { "msSFU30MaxUidNumber", "msSFU30MaxGidNumber", NULL };
  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 10, 0 };

  int rc = ldap_search_ext_s (ld, yp_dn, LDAP_SCOPE_BASE,
                               "(objectClass=*)", (char **) attrs, 0,
                               NULL, NULL, &tv, 1, &result);
  g_free (yp_dn);

  /* NO_SUCH_OBJECT is normal — many Samba 4 installs don't populate this. */
  if (rc == LDAP_SUCCESS && result) {
    LDAPMessage *entry = ldap_first_entry (ld, result);
    if (entry) {
      struct berval **bv;

      bv = ldap_get_values_len (ld, entry, "msSFU30MaxUidNumber");
      if (bv && bv[0])
        d->base->next_uid_hint = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "msSFU30MaxGidNumber");
      if (bv && bv[0])
        d->base->next_gid_hint = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
      ldap_value_free_len (bv);

      /* If we found anything, update source attribution. */
      if (d->base->next_uid_hint >= 0 || d->base->next_gid_hint >= 0) {
        g_free (d->base->source);
        d->base->source = g_strdup ("msSFU30 + profile config");
      }
    }
  }
  if (result) ldap_msgfree (result);
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
