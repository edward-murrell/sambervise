#include "sbv-collisions.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* Frees a single SbvCollisionHit. */
void
sbv_collision_hit_free (SbvCollisionHit *hit)
{
  if (!hit) return;
  g_free (hit->dn);
  g_free (hit->sam);
  g_free (hit);
}

/* Convenience over g_ptr_array_unref to keep the caller code readable. */
void
sbv_collision_hits_free (GPtrArray *hits)
{
  if (hits) g_ptr_array_unref (hits);
}

/* Per-task work parameters captured before dispatching to a worker. */
typedef struct {
  char   *filter;       /* fully-formed LDAP filter, owned        */
  char   *exclude_dn;   /* may be NULL                            */
} CheckData;

/* Frees CheckData previously passed via g_task_set_task_data. */
static void
check_data_free (CheckData *d)
{
  g_free (d->filter);
  g_free (d->exclude_dn);
  g_free (d);
}

/* Worker thread: runs a sub-tree LDAP search with the supplied filter and
 * collects matching DN/sam pairs, dropping the exclude_dn entry. The
 * function is shared between the UID and GID check entry points — only
 * the filter differs. */
static void
check_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  CheckData     *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  const char *base    = sbv_connection_get_base_dn (conn);
  const char *attrs[] = { "distinguishedName", "sAMAccountName", NULL };

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 10, 0 };

  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_SUBTREE,
                               d->filter, (char **) attrs, 0,
                               NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Collision check failed: %s",
                             ldap_err2string (rc));
    return;
  }

  GPtrArray *hits = g_ptr_array_new_with_free_func (
    (GDestroyNotify) sbv_collision_hit_free);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      struct berval **bv = ldap_get_values_len (ld, entry, "distinguishedName");
      if (!bv || !bv[0]) {
        if (bv) ldap_value_free_len (bv);
        continue;
      }

      char *dn = g_strndup (bv[0]->bv_val, bv[0]->bv_len);
      ldap_value_free_len (bv);

      if (d->exclude_dn && g_str_equal (dn, d->exclude_dn)) {
        g_free (dn);
        continue;
      }

      SbvCollisionHit *hit = g_new0 (SbvCollisionHit, 1);
      hit->dn = dn;

      bv = ldap_get_values_len (ld, entry, "sAMAccountName");
      if (bv && bv[0])
        hit->sam = g_strndup (bv[0]->bv_val, bv[0]->bv_len);
      ldap_value_free_len (bv);

      g_ptr_array_add (hits, hit);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, hits, (GDestroyNotify) g_ptr_array_unref);
}

/* Builds a CheckData with the supplied filter and dispatches the worker. */
static void
dispatch_check (SbvConnection       *conn,
                char                *filter,         /* takes ownership */
                const char          *exclude_dn,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  CheckData *d = g_new0 (CheckData, 1);
  d->filter     = filter;
  d->exclude_dn = g_strdup (exclude_dn);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) check_data_free);
  g_task_run_in_thread (task, check_thread);
  g_object_unref (task);
}

/* Public entry — see header. */
void
sbv_collisions_uid_check_async (SbvConnection       *conn,
                                 gint64               uid,
                                 const char          *exclude_dn,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  char *filter = g_strdup_printf (
    "(&(objectClass=user)(uidNumber=%" G_GINT64_FORMAT "))", uid);
  dispatch_check (conn, filter, exclude_dn, cancellable, callback, user_data);
}

/* Returns the GPtrArray<SbvCollisionHit*> from a UID check, or NULL on
 * error. Caller frees with sbv_collision_hits_free (or g_ptr_array_unref). */
GPtrArray *
sbv_collisions_uid_check_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* Public entry — see header. */
void
sbv_collisions_gid_check_async (SbvConnection       *conn,
                                 gint64               gid,
                                 const char          *exclude_dn,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  char *filter = g_strdup_printf (
    "(&(objectClass=group)(gidNumber=%" G_GINT64_FORMAT "))", gid);
  dispatch_check (conn, filter, exclude_dn, cancellable, callback, user_data);
}

/* Returns the GPtrArray<SbvCollisionHit*> from a GID check, or NULL on
 * error. Caller frees with sbv_collision_hits_free. */
GPtrArray *
sbv_collisions_gid_check_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Next-free-id scan ─────────────────────────────────────────────────── */

typedef struct {
  char  *attr_name;       /* "uidNumber" / "gidNumber"           */
  char  *object_class;    /* "user" / "group"                    */
  gint64 range_min;
  gint64 range_max;
} NextFreeData;

/* GCompareFunc that orders two gint64 values referenced by pointer. Used
 * to sort the GArray of taken values. */
static int
cmp_int64 (const void *a, const void *b)
{
  gint64 x = *(const gint64 *) a;
  gint64 y = *(const gint64 *) b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Frees NextFreeData previously passed via g_task_set_task_data. */
static void
next_free_data_free (NextFreeData *d)
{
  g_free (d->attr_name);
  g_free (d->object_class);
  g_free (d);
}

/* Worker thread: reads every <attr> value within [range_min, range_max]
 * for the given objectClass, then walks min..max returning the first
 * integer not in the used set. Returns -1 if the whole range is taken.
 *
 * gint64 results are funnelled through GTask's pointer payload using
 * GINT_TO_POINTER (cast to long via intptr) since GTask doesn't have a
 * native int64 propagate path. We allocate a one-int64 heap cell to
 * carry the value safely on both 32- and 64-bit targets. */
static void
next_free_thread (GTask *task, gpointer source, gpointer task_data,
                  GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  NextFreeData  *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  const char *base = sbv_connection_get_base_dn (conn);
  /* Bound the search to the range so we don't read every uidNumber on the
   * tree just to discard most of them. */
  char *filter = g_strdup_printf (
    "(&(objectClass=%s)(%s>=%" G_GINT64_FORMAT ")(%s<=%" G_GINT64_FORMAT "))",
    d->object_class,
    d->attr_name, d->range_min,
    d->attr_name, d->range_max);

  const char *attrs[] = { d->attr_name, NULL };

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 30, 0 };

  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_SUBTREE,
                               filter, (char **) attrs, 0,
                               NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  g_free (filter);

  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Range scan failed: %s", ldap_err2string (rc));
    return;
  }

  /* Collect taken values into a flat array; we'll sort and lockstep-walk
   * the sorted values against the range to find the first gap. */
  GArray *taken = g_array_new (FALSE, FALSE, sizeof (gint64));

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      struct berval **bv = ldap_get_values_len (ld, entry, d->attr_name);
      if (bv && bv[0]) {
        gint64 v = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
        g_array_append_val (taken, v);
      }
      ldap_value_free_len (bv);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  /* Sort ascending, then walk both sequences in lockstep. */
  g_array_sort (taken, (GCompareFunc) cmp_int64);

  gint64 found     = -1;
  guint  idx       = 0;
  for (gint64 candidate = d->range_min; candidate <= d->range_max; candidate++) {
    while (idx < taken->len && g_array_index (taken, gint64, idx) < candidate)
      idx++;
    if (idx >= taken->len || g_array_index (taken, gint64, idx) != candidate) {
      found = candidate;
      break;
    }
    idx++;
  }

  g_array_free (taken, TRUE);

  gint64 *boxed = g_new (gint64, 1);
  *boxed = found;
  g_task_return_pointer (task, boxed, g_free);
}

/* Public entry — see header. */
void
sbv_collisions_next_free_uid_async (SbvConnection       *conn,
                                     gint64               range_min,
                                     gint64               range_max,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  NextFreeData *d = g_new0 (NextFreeData, 1);
  d->attr_name    = g_strdup ("uidNumber");
  d->object_class = g_strdup ("user");
  d->range_min    = range_min;
  d->range_max    = range_max;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) next_free_data_free);
  g_task_run_in_thread (task, next_free_thread);
  g_object_unref (task);
}

/* Returns the lowest unused uidNumber in the range, or -1 if the range is
 * fully populated. On error, returns -1 and sets `error`. */
gint64
sbv_collisions_next_free_uid_finish (SbvConnection *conn,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  (void) conn;
  gint64 *boxed = g_task_propagate_pointer (G_TASK (result), error);
  if (!boxed) return -1;
  gint64 v = *boxed;
  g_free (boxed);
  return v;
}

/* Public entry — see header. */
void
sbv_collisions_next_free_gid_async (SbvConnection       *conn,
                                     gint64               range_min,
                                     gint64               range_max,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  NextFreeData *d = g_new0 (NextFreeData, 1);
  d->attr_name    = g_strdup ("gidNumber");
  d->object_class = g_strdup ("group");
  d->range_min    = range_min;
  d->range_max    = range_max;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) next_free_data_free);
  g_task_run_in_thread (task, next_free_thread);
  g_object_unref (task);
}

/* Returns the lowest unused gidNumber in the range, or -1 if exhausted. */
gint64
sbv_collisions_next_free_gid_finish (SbvConnection *conn,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  (void) conn;
  gint64 *boxed = g_task_propagate_pointer (G_TASK (result), error);
  if (!boxed) return -1;
  gint64 v = *boxed;
  g_free (boxed);
  return v;
}
