#include "sbv-pso-backend.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <string.h>

#define ATTR_APPLIES_TO "msDS-PSOAppliesTo"

/* The single-valued settings, in the order scalar_value() fills them. */
static const char *const SCALAR_ATTRS[] = {
  "msDS-PasswordSettingsPrecedence",
  "msDS-PasswordReversibleEncryptionEnabled",
  "msDS-PasswordHistoryLength",
  "msDS-PasswordComplexityEnabled",
  "msDS-MinimumPasswordLength",
  "msDS-MinimumPasswordAge",
  "msDS-MaximumPasswordAge",
  "msDS-LockoutThreshold",
  "msDS-LockoutObservationWindow",
  "msDS-LockoutDuration",
};
#define N_SCALARS G_N_ELEMENTS (SCALAR_ATTRS)

/* Writes the LDAP value of setting `i` of `p` into `buf`. */
static void
scalar_value (const SbvPso *p, guint i, char buf[32])
{
  const SbvDomainPolicy *pol = &p->policy;
  gint64 v = 0;
  switch (i) {
  case 0: v = p->precedence; break;
  case 1: g_strlcpy (buf, p->reversible ? "TRUE" : "FALSE", 32); return;
  case 2: v = pol->pwd_history_length; break;
  case 3: g_strlcpy (buf, (pol->pwd_properties & SBV_PWD_COMPLEX) ? "TRUE" : "FALSE", 32); return;
  case 4: v = pol->min_pwd_length; break;
  case 5: v = pol->min_pwd_age; break;
  case 6: v = pol->max_pwd_age; break;
  case 7: v = pol->lockout_threshold; break;
  case 8: v = pol->lockout_observation_window; break;
  case 9: v = pol->lockout_duration; break;
  default: g_assert_not_reached ();
  }
  g_snprintf (buf, 32, "%" G_GINT64_FORMAT, v);
}

/* Returns the PSO container's DN for `conn`. Caller frees. */
static char *
container_dn (SbvConnection *conn)
{
  return g_strdup_printf ("CN=Password Settings Container,CN=System,%s",
                          sbv_connection_get_base_dn (conn));
}

/* Builds an error from `rc` plus the server's diagnostic text, if any.
 * Caller holds the LDAP lock. */
static GError *
ldap_error (LDAP *ld, int rc, const char *what)
{
  char *diag = NULL;
  ldap_get_option (ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag);
  GError *err = g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "%s: %s%s%s%s", what,
                             ldap_err2string (rc),
                             diag && *diag ? " (" : "",
                             diag && *diag ? diag : "",
                             diag && *diag ? ")" : "");
  if (diag) ldap_memfree (diag);
  return err;
}

/* ── Modification lists ─────────────────────────────────────────────────── */

/* Frees one LDAPMod made by add_mod. Ignores the NULL terminator. */
static void
mod_free (gpointer data)
{
  LDAPMod *m = data;
  if (!m) return;
  g_free (m->mod_type);
  g_strfreev (m->mod_values);
  g_free (m);
}

/* Appends a modification. Takes ownership of `values`, which may be NULL
 * for a delete of every value. */
static void
add_mod (GPtrArray *mods, int op, const char *attr, char **values)
{
  LDAPMod *m    = g_new0 (LDAPMod, 1);
  m->mod_op     = op;
  m->mod_type   = g_strdup (attr);
  m->mod_values = values;
  g_ptr_array_add (mods, m);
}

/* Makes a one-value vector. */
static char **
one_value (const char *v)
{
  char **vals = g_new0 (char *, 2);
  vals[0] = g_strdup (v);
  return vals;
}

/* Returns a NULL-terminated LDAPMod array that borrows from `mods`. */
static LDAPMod **
mods_terminated (GPtrArray *mods)
{
  g_ptr_array_add (mods, NULL);
  return (LDAPMod **) mods->pdata;
}

/* ── List ───────────────────────────────────────────────────────────────── */

/* Returns `attr` of `entry` as a string, or NULL. Caller frees. */
static char *
entry_str (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  struct berval **bv  = ldap_get_values_len (ld, entry, attr);
  char           *out = (bv && bv[0]) ? g_strndup (bv[0]->bv_val, bv[0]->bv_len) : NULL;
  ldap_value_free_len (bv);
  return out;
}

/* Returns `attr` of `entry` as an integer, or 0 when missing. */
static gint64
entry_int (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  char  *s = entry_str (ld, entry, attr);
  gint64 v = s ? g_ascii_strtoll (s, NULL, 10) : 0;
  g_free (s);
  return v;
}

/* Returns TRUE when the boolean `attr` of `entry` reads "TRUE". */
static gboolean
entry_bool (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  char    *s = entry_str (ld, entry, attr);
  gboolean v = s && g_ascii_strcasecmp (s, "TRUE") == 0;
  g_free (s);
  return v;
}

/* Turns one search entry into a policy. */
static SbvPso *
pso_from_entry (LDAP *ld, LDAPMessage *entry)
{
  SbvPso *p = sbv_pso_new (NULL);
  char   *dn = ldap_get_dn (ld, entry);
  p->dn = g_strdup (dn);
  ldap_memfree (dn);

  p->name        = entry_str (ld, entry, "cn");
  if (!p->name) p->name = sbv_dn_rdn_value (p->dn);
  p->description = entry_str (ld, entry, "description");
  p->precedence  = entry_int (ld, entry, SCALAR_ATTRS[0]);
  p->reversible  = entry_bool (ld, entry, SCALAR_ATTRS[1]);

  SbvDomainPolicy *pol = &p->policy;
  pol->pwd_history_length         = entry_int (ld, entry, SCALAR_ATTRS[2]);
  pol->pwd_properties             = entry_bool (ld, entry, SCALAR_ATTRS[3]) ? SBV_PWD_COMPLEX : 0;
  pol->min_pwd_length             = entry_int (ld, entry, SCALAR_ATTRS[4]);
  pol->min_pwd_age                = entry_int (ld, entry, SCALAR_ATTRS[5]);
  pol->max_pwd_age                = entry_int (ld, entry, SCALAR_ATTRS[6]);
  pol->lockout_threshold          = entry_int (ld, entry, SCALAR_ATTRS[7]);
  pol->lockout_observation_window = entry_int (ld, entry, SCALAR_ATTRS[8]);
  pol->lockout_duration           = entry_int (ld, entry, SCALAR_ATTRS[9]);

  struct berval **bv = ldap_get_values_len (ld, entry, ATTR_APPLIES_TO);
  if (bv) {
    int    n   = ldap_count_values_len (bv);
    char **dns = g_new0 (char *, n + 1);
    for (int i = 0; i < n; i++)
      dns[i] = g_strndup (bv[i]->bv_val, bv[i]->bv_len);
    p->applies_to = dns;
    ldap_value_free_len (bv);
  }
  return p;
}

/* Worker: one-level search of the container. Runs on a GTask thread. */
static void
list_thread (GTask *task, gpointer source, gpointer task_data,
             GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  (void) task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  const char *attrs[N_SCALARS + 4];
  guint       n = 0;
  attrs[n++] = "cn";
  attrs[n++] = "description";
  attrs[n++] = ATTR_APPLIES_TO;
  for (guint i = 0; i < N_SCALARS; i++) attrs[n++] = SCALAR_ATTRS[i];
  attrs[n] = NULL;

  char          *base = container_dn (conn);
  struct timeval tv   = { 30, 0 };
  LDAPMessage   *res  = NULL;
  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_ONELEVEL,
                              "(objectClass=msDS-PasswordSettings)",
                              (char **) attrs, 0, NULL, NULL, &tv,
                              LDAP_NO_LIMIT, &res);
  g_free (base);

  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_pso_free);
  if (rc == LDAP_NO_SUCH_OBJECT) {
    if (res) ldap_msgfree (res);
    sbv_connection_release_ldap (conn);
    g_task_return_pointer (task, list, (GDestroyNotify) g_ptr_array_unref);
    return;
  }
  if (rc != LDAP_SUCCESS) {
    GError *err = ldap_error (ld, rc, "Couldn't list password policies");
    if (res) ldap_msgfree (res);
    sbv_connection_release_ldap (conn);
    g_ptr_array_unref (list);
    g_task_return_error (task, err);
    return;
  }

  for (LDAPMessage *e = ldap_first_entry (ld, res); e; e = ldap_next_entry (ld, e))
    g_ptr_array_add (list, pso_from_entry (ld, e));
  ldap_msgfree (res);
  sbv_connection_release_ldap (conn);

  g_ptr_array_sort (list, sbv_pso_compare);
  g_task_return_pointer (task, list, (GDestroyNotify) g_ptr_array_unref);
}

void
sbv_pso_list_async (SbvConnection       *conn,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

GPtrArray *
sbv_pso_list_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Create ─────────────────────────────────────────────────────────────── */

/* Returns a stripped copy of the policy's name. Caller frees. */
static char *
clean_name (const SbvPso *p)
{
  return g_strstrip (g_strdup (p->name ? p->name : ""));
}

/* Returns TRUE when `s` holds more than white space. */
static gboolean
has_text (const char *s)
{
  if (!s) return FALSE;
  for (; *s; s++)
    if (!g_ascii_isspace (*s)) return TRUE;
  return FALSE;
}

/* Worker: one add with every setting. Runs on a GTask thread. */
static void
create_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  SbvPso        *p    = task_data;
  (void) cancellable;

  char *name   = clean_name (p);
  char *parent = container_dn (conn);
  char *dn     = sbv_dn_build ("CN", name, parent);
  g_free (parent);

  GPtrArray *mods = g_ptr_array_new_with_free_func (mod_free);
  add_mod (mods, LDAP_MOD_ADD, "objectClass", one_value ("msDS-PasswordSettings"));
  if (has_text (p->description))
    add_mod (mods, LDAP_MOD_ADD, "description", one_value (p->description));
  for (guint i = 0; i < N_SCALARS; i++) {
    char buf[32];
    scalar_value (p, i, buf);
    add_mod (mods, LDAP_MOD_ADD, SCALAR_ATTRS[i], one_value (buf));
  }
  if (p->applies_to && p->applies_to[0])
    add_mod (mods, LDAP_MOD_ADD, ATTR_APPLIES_TO, g_strdupv (p->applies_to));
  g_free (name);

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_ptr_array_unref (mods);
    g_free (dn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  guint   len = mods->len;
  int     rc  = ldap_add_ext_s (ld, dn, mods_terminated (mods), NULL, NULL);
  GError *err = rc == LDAP_SUCCESS ? NULL
                                   : ldap_error (ld, rc, "Couldn't create the policy");
  sbv_connection_release_ldap (conn);
  g_ptr_array_set_size (mods, len);
  g_ptr_array_unref (mods);

  if (err) {
    g_free (dn);
    g_task_return_error (task, err);
  } else {
    g_task_return_pointer (task, dn, g_free);
  }
}

void
sbv_pso_create_async (SbvConnection       *conn,
                      const SbvPso        *pso,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, sbv_pso_copy (pso), (GDestroyNotify) sbv_pso_free);
  g_task_run_in_thread (task, create_thread);
  g_object_unref (task);
}

char *
sbv_pso_create_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Update ─────────────────────────────────────────────────────────────── */

typedef struct {
  SbvPso *original;
  SbvPso *updated;
} UpdateData;

/* Frees an UpdateData. */
static void
update_data_free (UpdateData *d)
{
  sbv_pso_free (d->original);
  sbv_pso_free (d->updated);
  g_free (d);
}

/* Returns the DNs of `from` that are missing from `other`, or NULL when
 * there are none. Caller frees. */
static char **
dns_missing (char **from, char **other)
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; from && from[i]; i++) {
    gboolean found = FALSE;
    for (guint j = 0; other && other[j] && !found; j++)
      found = sbv_dn_equal (from[i], other[j]);
    if (!found) g_ptr_array_add (out, g_strdup (from[i]));
  }
  if (out->len == 0) {
    g_ptr_array_unref (out);
    return NULL;
  }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

/* Worker: one modify for the changed settings, then a rename. Runs on a
 * GTask thread. */
static void
update_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  UpdateData    *d    = task_data;
  SbvPso        *o    = d->original;
  SbvPso        *u    = d->updated;
  (void) cancellable;

  GPtrArray *mods = g_ptr_array_new_with_free_func (mod_free);
  for (guint i = 0; i < N_SCALARS; i++) {
    char a[32], b[32];
    scalar_value (o, i, a);
    scalar_value (u, i, b);
    if (strcmp (a, b) != 0)
      add_mod (mods, LDAP_MOD_REPLACE, SCALAR_ATTRS[i], one_value (b));
  }

  gboolean had_desc = has_text (o->description);
  gboolean has_desc = has_text (u->description);
  if (has_desc && (!had_desc || strcmp (o->description, u->description) != 0))
    add_mod (mods, LDAP_MOD_REPLACE, "description", one_value (u->description));
  else if (!has_desc && had_desc)
    add_mod (mods, LDAP_MOD_DELETE, "description", NULL);

  char **gone  = dns_missing (o->applies_to, u->applies_to);
  char **added = dns_missing (u->applies_to, o->applies_to);
  if (gone)  add_mod (mods, LDAP_MOD_DELETE, ATTR_APPLIES_TO, gone);
  if (added) add_mod (mods, LDAP_MOD_ADD,    ATTR_APPLIES_TO, added);

  char    *old_name = clean_name (o);
  char    *new_name = clean_name (u);
  gboolean rename   = strcmp (old_name, new_name) != 0;
  g_free (old_name);

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_ptr_array_unref (mods);
    g_free (new_name);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  GError *err = NULL;
  if (mods->len > 0) {
    guint len = mods->len;
    int   rc  = ldap_modify_ext_s (ld, o->dn, mods_terminated (mods), NULL, NULL);
    g_ptr_array_set_size (mods, len);
    if (rc != LDAP_SUCCESS)
      err = ldap_error (ld, rc, "Couldn't save the policy");
  }
  g_ptr_array_unref (mods);

  char *dn = g_strdup (o->dn);
  if (!err && rename) {
    char *rdn    = sbv_dn_build ("CN", new_name, NULL);
    char *parent = NULL;
    sbv_dn_split (o->dn, NULL, &parent);
    int rc = ldap_rename_s (ld, o->dn, rdn, NULL, 1, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      err = ldap_error (ld, rc, "Saved the settings, but couldn't rename the policy");
    } else {
      g_free (dn);
      dn = sbv_dn_build ("CN", new_name, parent);
    }
    g_free (rdn);
    g_free (parent);
  }
  sbv_connection_release_ldap (conn);
  g_free (new_name);

  if (err) {
    g_free (dn);
    g_task_return_error (task, err);
  } else {
    g_task_return_pointer (task, dn, g_free);
  }
}

void
sbv_pso_update_async (SbvConnection       *conn,
                      const SbvPso        *original,
                      const SbvPso        *updated,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask      *task = g_task_new (conn, cancellable, callback, user_data);
  UpdateData *d    = g_new0 (UpdateData, 1);
  d->original = sbv_pso_copy (original);
  d->updated  = sbv_pso_copy (updated);
  g_task_set_task_data (task, d, (GDestroyNotify) update_data_free);
  g_task_run_in_thread (task, update_thread);
  g_object_unref (task);
}

char *
sbv_pso_update_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

/* Worker: deletes the policy object. Runs on a GTask thread. */
static void
delete_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  const char    *dn   = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }
  int     rc  = ldap_delete_ext_s (ld, dn, NULL, NULL);
  GError *err = rc == LDAP_SUCCESS ? NULL
                                   : ldap_error (ld, rc, "Couldn't delete the policy");
  sbv_connection_release_ldap (conn);
  if (err) g_task_return_error (task, err);
  else     g_task_return_boolean (task, TRUE);
}

void
sbv_pso_delete_async (SbvConnection       *conn,
                      const char          *dn,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (dn), g_free);
  g_task_run_in_thread (task, delete_thread);
  g_object_unref (task);
}

gboolean
sbv_pso_delete_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
