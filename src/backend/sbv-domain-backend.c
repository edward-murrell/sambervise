#include "sbv-domain-backend.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <string.h>

/* ── Helpers (caller holds the LDAP lock) ───────────────────────────────── */

/* Base-scope search of `dn` for `attrs`. Returns the entry's message, or
 * NULL with `*rc` set. Caller frees with ldap_msgfree. */
static LDAPMessage *
read_base (LDAP *ld, const char *dn, char **attrs, int *rc)
{
  struct timeval tv  = { 30, 0 };
  LDAPMessage   *res = NULL;
  *rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                           attrs, 0, NULL, NULL, &tv, 1, &res);
  if (*rc != LDAP_SUCCESS) {
    if (res) ldap_msgfree (res);
    return NULL;
  }
  if (!ldap_first_entry (ld, res)) {
    ldap_msgfree (res);
    *rc = LDAP_NO_SUCH_OBJECT;
    return NULL;
  }
  return res;
}

/* Returns the first value of `attr` on the result's entry. Caller frees. */
static char *
first_value (LDAP *ld, LDAPMessage *res, const char *attr)
{
  LDAPMessage    *entry = ldap_first_entry (ld, res);
  struct berval **bv    = entry ? ldap_get_values_len (ld, entry, attr) : NULL;
  char           *out   = (bv && bv[0]) ? g_strndup (bv[0]->bv_val, bv[0]->bv_len) : NULL;
  ldap_value_free_len (bv);
  return out;
}

/* Returns `attr` as an integer, or `fallback` when it is missing. */
static gint64
int_value (LDAP *ld, LDAPMessage *res, const char *attr, gint64 fallback)
{
  char  *s = first_value (ld, res, attr);
  gint64 v = s ? g_ascii_strtoll (s, NULL, 10) : fallback;
  g_free (s);
  return v;
}

/* ── Policy read ────────────────────────────────────────────────────────── */

/* Worker: reads the policy attributes from the domain root. Runs on a
 * GTask thread. */
static void
policy_read_thread (GTask *task, gpointer source, gpointer task_data,
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

  char *attrs[] = {
    (char *) "minPwdLength", (char *) "pwdHistoryLength",
    (char *) "maxPwdAge", (char *) "minPwdAge", (char *) "pwdProperties",
    (char *) "lockoutThreshold", (char *) "lockoutDuration",
    (char *) "lockoutObservationWindow", NULL
  };
  int          rc  = LDAP_SUCCESS;
  LDAPMessage *res = read_base (ld, sbv_connection_get_base_dn (conn), attrs, &rc);
  if (!res) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Couldn't read the domain policy: %s",
                             ldap_err2string (rc));
    return;
  }

  SbvDomainPolicy *p = g_new0 (SbvDomainPolicy, 1);
  p->min_pwd_length             = int_value (ld, res, "minPwdLength", 0);
  p->pwd_history_length         = int_value (ld, res, "pwdHistoryLength", 0);
  p->max_pwd_age                = int_value (ld, res, "maxPwdAge", 0);
  p->min_pwd_age                = int_value (ld, res, "minPwdAge", 0);
  p->pwd_properties             = int_value (ld, res, "pwdProperties", 0);
  p->lockout_threshold          = int_value (ld, res, "lockoutThreshold", 0);
  p->lockout_duration           = int_value (ld, res, "lockoutDuration", 0);
  p->lockout_observation_window = int_value (ld, res, "lockoutObservationWindow", 0);
  ldap_msgfree (res);
  sbv_connection_release_ldap (conn);
  g_task_return_pointer (task, p, g_free);
}

void
sbv_domain_policy_read_async (SbvConnection       *conn,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, policy_read_thread);
  g_object_unref (task);
}

SbvDomainPolicy *
sbv_domain_policy_read_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Policy write ───────────────────────────────────────────────────────── */

typedef struct {
  SbvDomainPolicy original;
  SbvDomainPolicy updated;
} WriteData;

/* Worker: sends one modify with a replace for each changed attribute.
 * Runs on a GTask thread. */
static void
policy_write_thread (GTask *task, gpointer source, gpointer task_data,
                     GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  WriteData     *d    = task_data;
  (void) cancellable;

  struct {
    const char *attr;
    gint64      old_value;
    gint64      new_value;
  } fields[] = {
    { "minPwdLength",             d->original.min_pwd_length,             d->updated.min_pwd_length },
    { "pwdHistoryLength",         d->original.pwd_history_length,         d->updated.pwd_history_length },
    { "maxPwdAge",                d->original.max_pwd_age,                d->updated.max_pwd_age },
    { "minPwdAge",                d->original.min_pwd_age,                d->updated.min_pwd_age },
    { "pwdProperties",            d->original.pwd_properties,             d->updated.pwd_properties },
    { "lockoutThreshold",         d->original.lockout_threshold,          d->updated.lockout_threshold },
    { "lockoutDuration",          d->original.lockout_duration,           d->updated.lockout_duration },
    { "lockoutObservationWindow", d->original.lockout_observation_window, d->updated.lockout_observation_window },
  };

  LDAPMod  mod_buf[G_N_ELEMENTS (fields)];
  LDAPMod *mods[G_N_ELEMENTS (fields) + 1];
  char     strs[G_N_ELEMENTS (fields)][32];
  char    *vals[G_N_ELEMENTS (fields)][2];
  int      n = 0;

  for (gsize i = 0; i < G_N_ELEMENTS (fields); i++) {
    if (fields[i].old_value == fields[i].new_value) continue;
    g_snprintf (strs[n], sizeof strs[n], "%" G_GINT64_FORMAT, fields[i].new_value);
    vals[n][0] = strs[n];
    vals[n][1] = NULL;
    mod_buf[n].mod_op     = LDAP_MOD_REPLACE;
    mod_buf[n].mod_type   = (char *) fields[i].attr;
    mod_buf[n].mod_values = vals[n];
    mods[n] = &mod_buf[n];
    n++;
  }
  mods[n] = NULL;

  if (n == 0) {
    g_task_return_boolean (task, TRUE);
    return;
  }

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  int   rc   = ldap_modify_ext_s (ld, sbv_connection_get_base_dn (conn), mods, NULL, NULL);
  char *diag = NULL;
  if (rc != LDAP_SUCCESS)
    ldap_get_option (ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Couldn't save the policy: %s%s%s%s",
                             ldap_err2string (rc),
                             diag && *diag ? " (" : "",
                             diag && *diag ? diag : "",
                             diag && *diag ? ")" : "");
  } else {
    g_task_return_boolean (task, TRUE);
  }
  if (diag) ldap_memfree (diag);
}

void
sbv_domain_policy_write_async (SbvConnection         *conn,
                               const SbvDomainPolicy *original,
                               const SbvDomainPolicy *updated,
                               GCancellable          *cancellable,
                               GAsyncReadyCallback    callback,
                               gpointer               user_data)
{
  GTask     *task = g_task_new (conn, cancellable, callback, user_data);
  WriteData *d    = g_new0 (WriteData, 1);
  d->original = *original;
  d->updated  = *updated;
  g_task_set_task_data (task, d, g_free);
  g_task_run_in_thread (task, policy_write_thread);
  g_object_unref (task);
}

gboolean
sbv_domain_policy_write_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── FSMO roles ─────────────────────────────────────────────────────────── */

/* Reads one role's holder from `dn` and adds it to `roles`. Optional
 * roles are left out when their object is missing. Others get a row
 * that says the holder couldn't be read. */
static void
add_role (LDAP *ld, GPtrArray *roles, const char *role, const char *dn,
          gboolean optional)
{
  char        *attrs[] = { (char *) "fSMORoleOwner", NULL };
  int          rc      = LDAP_SUCCESS;
  LDAPMessage *res     = dn ? read_base (ld, dn, attrs, &rc) : NULL;
  char        *holder  = res ? first_value (ld, res, "fSMORoleOwner") : NULL;
  if (res) ldap_msgfree (res);
  if (!holder && optional) return;

  SbvFsmoRole *r = g_new0 (SbvFsmoRole, 1);
  r->role      = g_strdup (role);
  r->holder_dn = holder;

  char *server_dn = NULL;
  if (holder && sbv_fsmo_parse_holder (holder, &r->server, &server_dn,
                                       &r->site, &r->deleted)
      && !r->deleted) {
    char        *host_attrs[] = { (char *) "dNSHostName", NULL };
    int          hrc          = LDAP_SUCCESS;
    LDAPMessage *hres         = read_base (ld, server_dn, host_attrs, &hrc);
    if (hres) {
      r->host = first_value (ld, hres, "dNSHostName");
      ldap_msgfree (hres);
    }
  }
  g_free (server_dn);
  g_ptr_array_add (roles, r);
}

/* Worker: reads the naming contexts from the root DSE, then each role's
 * holder. Runs on a GTask thread. */
static void
fsmo_read_thread (GTask *task, gpointer source, gpointer task_data,
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

  char *dse_attrs[] = { (char *) "configurationNamingContext",
                        (char *) "schemaNamingContext",
                        (char *) "rootDomainNamingContext", NULL };
  int          rc  = LDAP_SUCCESS;
  LDAPMessage *dse = read_base (ld, "", dse_attrs, &rc);
  if (!dse) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Couldn't read the root DSE: %s", ldap_err2string (rc));
    return;
  }
  char *config = first_value (ld, dse, "configurationNamingContext");
  char *schema = first_value (ld, dse, "schemaNamingContext");
  char *forest = first_value (ld, dse, "rootDomainNamingContext");
  ldap_msgfree (dse);

  const char *base = sbv_connection_get_base_dn (conn);
  if (!forest) forest = g_strdup (base);

  char *partitions = config ? g_strdup_printf ("CN=Partitions,%s", config) : NULL;
  char *rid        = g_strdup_printf ("CN=RID Manager$,CN=System,%s", base);
  char *infra      = g_strdup_printf ("CN=Infrastructure,%s", base);
  char *dom_dns    = g_strdup_printf ("CN=Infrastructure,DC=DomainDnsZones,%s", base);
  char *for_dns    = g_strdup_printf ("CN=Infrastructure,DC=ForestDnsZones,%s", forest);

  GPtrArray *roles = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_fsmo_role_free);
  add_role (ld, roles, "Schema Master",         schema,     FALSE);
  add_role (ld, roles, "Domain Naming Master",  partitions, FALSE);
  add_role (ld, roles, "PDC Emulator",          base,       FALSE);
  add_role (ld, roles, "RID Master",            rid,        FALSE);
  add_role (ld, roles, "Infrastructure Master", infra,      FALSE);
  add_role (ld, roles, "Domain DNS Zones Master", dom_dns,  TRUE);
  add_role (ld, roles, "Forest DNS Zones Master", for_dns,  TRUE);
  sbv_connection_release_ldap (conn);

  g_free (config);
  g_free (schema);
  g_free (forest);
  g_free (partitions);
  g_free (rid);
  g_free (infra);
  g_free (dom_dns);
  g_free (for_dns);
  g_task_return_pointer (task, roles, (GDestroyNotify) g_ptr_array_unref);
}

void
sbv_domain_fsmo_read_async (SbvConnection       *conn,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, fsmo_read_thread);
  g_object_unref (task);
}

GPtrArray *
sbv_domain_fsmo_read_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}
