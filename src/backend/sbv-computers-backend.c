#include "sbv-computers-backend.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

#define UAC_ACCOUNTDISABLE          0x0002
#define UAC_WORKSTATION_TRUST_ACCT  0x1000

/* ── List computers ─────────────────────────────────────────────────────── */

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
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  const char *base   = sbv_connection_get_base_dn (conn);
  const char *filter = "(objectClass=computer)";
  const char *attrs[] = {
    "distinguishedName", "sAMAccountName", "cn",
    "description", "dNSHostName",
    "operatingSystem", "operatingSystemVersion", "operatingSystemServicePack",
    "userAccountControl", "pwdLastSet", "servicePrincipalName",
    NULL
  };

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 30, 0 };

  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_SUBTREE,
                               filter, (char **) attrs, 0,
                               NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Search failed: %s", ldap_err2string (rc));
    return;
  }

  GListStore *store = g_list_store_new (SBV_TYPE_COMPUTER);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      SbvComputer    *computer = sbv_computer_new ();
      struct berval **bv;

#define GET_STR(attr, setter) \
      bv = ldap_get_values_len (ld, entry, (attr)); \
      if (bv && bv[0]) setter (computer, bv[0]->bv_val); \
      ldap_value_free_len (bv);

      GET_STR ("distinguishedName",          sbv_computer_set_dn)
      GET_STR ("sAMAccountName",             sbv_computer_set_sam)
      GET_STR ("cn",                         sbv_computer_set_cn)
      GET_STR ("description",                sbv_computer_set_description)
      GET_STR ("dNSHostName",                sbv_computer_set_dns_hostname)
      GET_STR ("operatingSystem",            sbv_computer_set_os)
      GET_STR ("operatingSystemVersion",     sbv_computer_set_os_version)
      GET_STR ("operatingSystemServicePack", sbv_computer_set_os_service_pack)

#undef GET_STR

      bv = ldap_get_values_len (ld, entry, "userAccountControl");
      if (bv && bv[0]) {
        gint64 uac = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
        sbv_computer_set_uac (computer, uac);
        sbv_computer_set_enabled (computer, !(uac & UAC_ACCOUNTDISABLE));
      }
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "pwdLastSet");
      if (bv && bv[0])
        sbv_computer_set_pwd_last_set (computer,
          g_ascii_strtoll (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

      /* servicePrincipalName — multi-valued */
      bv = ldap_get_values_len (ld, entry, "servicePrincipalName");
      if (bv) {
        int    count = ldap_count_values_len (bv);
        char **spns  = g_new0 (char *, count + 1);
        for (int i = 0; i < count; i++)
          spns[i] = g_strndup (bv[i]->bv_val, bv[i]->bv_len);
        sbv_computer_set_spn (computer, spns);
        ldap_value_free_len (bv);
      }

      /* Collect all raw LDAP attributes for display */
      {
        GHashTable *raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, (GDestroyNotify) g_strfreev);
        BerElement *ber2 = NULL;
        char *aname;
        for (aname = ldap_first_attribute (ld, entry, &ber2);
             aname != NULL;
             aname = ldap_next_attribute (ld, entry, ber2))
        {
          struct berval **av = ldap_get_values_len (ld, entry, aname);
          if (av) {
            int cnt = ldap_count_values_len (av);
            char **sv = g_new0 (char *, cnt + 1);
            for (int j = 0; j < cnt; j++) {
              sv[j] = g_utf8_validate (av[j]->bv_val, (gssize) av[j]->bv_len, NULL)
                ? g_strndup (av[j]->bv_val, av[j]->bv_len)
                : g_strdup_printf ("<binary %zu bytes>", av[j]->bv_len);
            }
            g_hash_table_insert (raw, g_strdup (aname), sv);
            ldap_value_free_len (av);
          }
          ldap_memfree (aname);
        }
        if (ber2) ber_free (ber2, 0);
        sbv_computer_set_ldap_attrs (computer, raw);
      }

      g_list_store_append (store, computer);
      g_object_unref (computer);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, store, g_object_unref);
}

void
sbv_computers_list_async (SbvConnection       *conn,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

GListStore *
sbv_computers_list_finish (SbvConnection *conn,
                            GAsyncResult  *result,
                            GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Enable / disable ───────────────────────────────────────────────────── */

typedef struct {
  char    *dn;
  gboolean enabled;
} SetEnabledData;

static void
set_enabled_data_free (SetEnabledData *d)
{
  g_free (d->dn);
  g_free (d);
}

static void
set_enabled_thread (GTask *task, gpointer source, gpointer task_data,
                    GCancellable *cancellable)
{
  SbvConnection  *conn = SBV_CONNECTION (source);
  SetEnabledData *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  const char    *attrs[] = { "userAccountControl", NULL };
  LDAPMessage   *result  = NULL;
  struct timeval tv      = { 10, 0 };

  int rc = ldap_search_ext_s (ld, d->dn, LDAP_SCOPE_BASE,
                               "(objectClass=*)", (char **) attrs, 0,
                               NULL, NULL, &tv, 1, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Read failed: %s", ldap_err2string (rc));
    return;
  }

  LDAPMessage *entry = ldap_first_entry (ld, result);
  long         uac   = 0;
  if (entry) {
    struct berval **bv = ldap_get_values_len (ld, entry, "userAccountControl");
    if (bv && bv[0])
      uac = strtol (bv[0]->bv_val, NULL, 10);
    ldap_value_free_len (bv);
  }
  ldap_msgfree (result);

  if (d->enabled)
    uac &= ~UAC_ACCOUNTDISABLE;
  else
    uac |= UAC_ACCOUNTDISABLE;

  char    uac_str[32];
  snprintf (uac_str, sizeof uac_str, "%ld", uac);
  char   *uac_vals[] = { uac_str, NULL };
  LDAPMod uac_mod    = { LDAP_MOD_REPLACE, "userAccountControl",
                         { .modv_strvals = uac_vals } };
  LDAPMod *mods[]    = { &uac_mod, NULL };

  rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Modify failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_computers_set_enabled_async (SbvConnection       *conn,
                                  SbvComputer         *computer,
                                  gboolean             enabled,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  SetEnabledData *d = g_new0 (SetEnabledData, 1);
  d->dn      = g_strdup (sbv_computer_get_dn (computer));
  d->enabled = enabled;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) set_enabled_data_free);
  g_task_run_in_thread (task, set_enabled_thread);
  g_object_unref (task);
}

gboolean
sbv_computers_set_enabled_finish (SbvConnection *conn,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Update attributes ──────────────────────────────────────────────────── */

typedef struct {
  char *dn;
  char *description;
  char *dns_hostname;
} UpdateAttrsData;

static void
update_attrs_data_free (UpdateAttrsData *d)
{
  g_free (d->dn);
  g_free (d->description);
  g_free (d->dns_hostname);
  g_free (d);
}

static void
update_attrs_thread (GTask *task, gpointer source, gpointer task_data,
                     GCancellable *cancellable)
{
  SbvConnection   *conn = SBV_CONNECTION (source);
  UpdateAttrsData *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  LDAPMod  mods_buf[2];
  LDAPMod *mods[3];
  int      n = 0;

  char *desc_vals[] = { d->description,  NULL };
  char *dns_vals[]  = { d->dns_hostname, NULL };

#define ADD_MOD(attr, val, vals_arr) \
  if ((val) != NULL) { \
    mods_buf[n] = (LDAPMod){ LDAP_MOD_REPLACE, (char *)(attr), \
                              { .modv_strvals = (*(val)) ? (vals_arr) : NULL } }; \
    mods[n] = &mods_buf[n]; \
    n++; \
  }

  ADD_MOD ("description", d->description,  desc_vals)
  ADD_MOD ("dNSHostName", d->dns_hostname, dns_vals)

#undef ADD_MOD

  mods[n] = NULL;

  int rc = (n > 0) ? ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL)
                   : LDAP_SUCCESS;
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Attribute update failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_computers_update_attrs_async (SbvConnection       *conn,
                                   SbvComputer         *computer,
                                   const char          *description,
                                   const char          *dns_hostname,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  UpdateAttrsData *d = g_new0 (UpdateAttrsData, 1);
  d->dn           = g_strdup (sbv_computer_get_dn (computer));
  d->description  = g_strdup (description);
  d->dns_hostname = g_strdup (dns_hostname);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) update_attrs_data_free);
  g_task_run_in_thread (task, update_attrs_thread);
  g_object_unref (task);
}

gboolean
sbv_computers_update_attrs_finish (SbvConnection *conn,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Set SPNs ───────────────────────────────────────────────────────────── */

typedef struct {
  char  *dn;
  char **spns;   /* NULL-terminated; may be NULL or empty for "delete all" */
} SetSpnData;

/* Frees the SetSpnData task payload. */
static void
set_spn_data_free (SetSpnData *d)
{
  g_free (d->dn);
  g_strfreev (d->spns);
  g_free (d);
}

/* Worker thread: replaces the full servicePrincipalName attribute. An
 * empty / NULL value list collapses to LDAP_MOD_REPLACE with no values,
 * which deletes the attribute. */
static void
set_spn_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  SetSpnData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  guint count = d->spns ? g_strv_length (d->spns) : 0;
  LDAPMod  spn_mod = { LDAP_MOD_REPLACE, "servicePrincipalName",
                       { .modv_strvals = (count > 0) ? d->spns : NULL } };
  LDAPMod *mods[]  = { &spn_mod, NULL };

  int rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "SPN update failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

/* Public entry — see header. Snapshots the SPN list and dispatches to a
 * worker thread. */
void
sbv_computers_set_spn_async (SbvConnection       *conn,
                              SbvComputer         *computer,
                              const char * const  *spns,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  SetSpnData *d = g_new0 (SetSpnData, 1);
  d->dn   = g_strdup (sbv_computer_get_dn (computer));
  d->spns = spns ? g_strdupv ((char **) spns) : NULL;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) set_spn_data_free);
  g_task_run_in_thread (task, set_spn_thread);
  g_object_unref (task);
}

/* Reports success/failure of a previously-scheduled SPN update. */
gboolean
sbv_computers_set_spn_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Create computer ────────────────────────────────────────────────────── */

typedef struct {
  char *cn;
  char *dns_hostname;
  char *description;
  char *container_dn;
} CreateComputerData;

/* Frees CreateComputerData previously passed via g_task_set_task_data. */
static void
create_computer_data_free (CreateComputerData *d)
{
  g_free (d->cn);
  g_free (d->dns_hostname);
  g_free (d->description);
  g_free (d->container_dn);
  g_free (d);
}

/* Worker thread: builds the LDAP entry and issues ldap_add_ext_s for a
 * new computer account. sAMAccountName is `<cn>$` per AD convention. The
 * account is created disabled and gets the WORKSTATION_TRUST_ACCOUNT bit
 * so AD treats it as a member workstation. */
static void
create_computer_thread (GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
  SbvConnection      *conn = SBV_CONNECTION (source);
  CreateComputerData *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *dn  = g_strdup_printf ("CN=%s,%s", d->cn, d->container_dn);
  char *sam = g_strdup_printf ("%s$", d->cn);

  char uac_buf[16];
  g_snprintf (uac_buf, sizeof uac_buf, "%d",
              UAC_WORKSTATION_TRUST_ACCT | UAC_ACCOUNTDISABLE);

  char *oc_vals[]   = { "top", "person", "organizationalPerson",
                        "user", "computer", NULL };
  char *cn_vals[]   = { d->cn, NULL };
  char *sam_vals[]  = { sam, NULL };
  char *uac_vals[]  = { uac_buf, NULL };
  char *dns_vals[]  = { d->dns_hostname, NULL };
  char *desc_vals[] = { d->description, NULL };

  LDAPMod oc_mod   = { LDAP_MOD_ADD, "objectClass",        { .modv_strvals = oc_vals  } };
  LDAPMod cn_mod   = { LDAP_MOD_ADD, "cn",                 { .modv_strvals = cn_vals  } };
  LDAPMod sam_mod  = { LDAP_MOD_ADD, "sAMAccountName",     { .modv_strvals = sam_vals } };
  LDAPMod uac_mod  = { LDAP_MOD_ADD, "userAccountControl", { .modv_strvals = uac_vals } };
  LDAPMod dns_mod  = { LDAP_MOD_ADD, "dNSHostName",        { .modv_strvals = dns_vals } };
  LDAPMod desc_mod = { LDAP_MOD_ADD, "description",        { .modv_strvals = desc_vals } };

  LDAPMod *mods[7];
  int      n = 0;
  mods[n++] = &oc_mod;
  mods[n++] = &cn_mod;
  mods[n++] = &sam_mod;
  mods[n++] = &uac_mod;
  if (d->dns_hostname && *d->dns_hostname) mods[n++] = &dns_mod;
  if (d->description  && *d->description)  mods[n++] = &desc_mod;
  mods[n] = NULL;

  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  g_free (sam);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_free (dn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Create failed: %s", ldap_err2string (rc));
    return;
  }

  g_task_return_pointer (task, dn, g_free);
}

/* Public entry — see header. Validates required fields and dispatches to a
 * worker thread. */
void
sbv_computers_create_async (SbvConnection       *conn,
                             const char          *cn,
                             const char          *dns_hostname,
                             const char          *description,
                             const char          *container_dn,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  if (!cn || !*cn || !container_dn || !*container_dn) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Computer name and container are required");
    g_object_unref (task);
    return;
  }

  CreateComputerData *d = g_new0 (CreateComputerData, 1);
  d->cn           = g_strdup (cn);
  d->dns_hostname = g_strdup (dns_hostname);
  d->description  = g_strdup (description);
  d->container_dn = g_strdup (container_dn);

  g_task_set_task_data (task, d, (GDestroyNotify) create_computer_data_free);
  g_task_run_in_thread (task, create_computer_thread);
  g_object_unref (task);
}

/* Returns the DN of the newly-created computer (caller frees) or NULL on
 * error. */
char *
sbv_computers_create_finish (SbvConnection *conn,
                              GAsyncResult  *result,
                              GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Delete computer ────────────────────────────────────────────────────── */

/* Worker thread: issues a single ldap_delete_ext_s call. */
static void
delete_computer_thread (GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  char          *dn   = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  int rc = ldap_delete_ext_s (ld, dn, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Delete failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

/* Public entry — schedules the delete on a worker thread. The computer's
 * DN is captured at call time so the SbvComputer may be freed before
 * completion. */
void
sbv_computers_delete_async (SbvConnection       *conn,
                             SbvComputer         *computer,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (sbv_computer_get_dn (computer)),
                         g_free);
  g_task_run_in_thread (task, delete_computer_thread);
  g_object_unref (task);
}

/* Reports success/failure of a previously-scheduled delete. */
gboolean
sbv_computers_delete_finish (SbvConnection *conn,
                              GAsyncResult  *result,
                              GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
