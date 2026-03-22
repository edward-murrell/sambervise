#include "sbv-users-backend.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

#define UAC_ACCOUNTDISABLE 0x0002
#define UAC_NORMAL_ACCOUNT 0x0200

/* ── List users ─────────────────────────────────────────────────────────── */

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

  const char *base = sbv_connection_get_base_dn (conn);
  const char *filter = "(&(objectClass=user)(!(objectClass=computer)))";
  const char *attrs[] = {
    "distinguishedName", "sAMAccountName", "cn", "mail",
    "description", "userAccountControl", NULL
  };

  LDAPMessage *result = NULL;
  struct timeval tv = { 30, 0 };

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

  GListStore *store = g_list_store_new (SBV_TYPE_USER);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      SbvUser *user = sbv_user_new ();

      struct berval **bv;

#define GET_STR(attr, setter) \
      bv = ldap_get_values_len (ld, entry, (attr)); \
      if (bv && bv[0]) setter (user, bv[0]->bv_val); \
      ldap_value_free_len (bv);

      GET_STR ("distinguishedName", sbv_user_set_dn)
      GET_STR ("sAMAccountName",    sbv_user_set_sam)
      GET_STR ("cn",                sbv_user_set_display_name)
      GET_STR ("mail",              sbv_user_set_email)
      GET_STR ("description",       sbv_user_set_description)

#undef GET_STR

      bv = ldap_get_values_len (ld, entry, "userAccountControl");
      if (bv && bv[0]) {
        long uac = strtol (bv[0]->bv_val, NULL, 10);
        sbv_user_set_enabled (user, !(uac & UAC_ACCOUNTDISABLE));
      }
      ldap_value_free_len (bv);

      g_list_store_append (store, user);
      g_object_unref (user);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, store, g_object_unref);
}

void
sbv_users_list_async (SbvConnection       *conn,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

GListStore *
sbv_users_list_finish (SbvConnection *conn,
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

  /* Read current userAccountControl */
  const char *attrs[] = { "userAccountControl", NULL };
  LDAPMessage *result = NULL;
  struct timeval tv = { 10, 0 };

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
  long uac = UAC_NORMAL_ACCOUNT;
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

  char uac_str[32];
  snprintf (uac_str, sizeof uac_str, "%ld", uac);

  char *uac_vals[]  = { uac_str, NULL };
  LDAPMod uac_mod   = { LDAP_MOD_REPLACE, "userAccountControl", { .modv_strvals = uac_vals } };
  LDAPMod *mods[]   = { &uac_mod, NULL };

  rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Modify failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_users_set_enabled_async (SbvConnection       *conn,
                              SbvUser             *user,
                              gboolean             enabled,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  SetEnabledData *d = g_new0 (SetEnabledData, 1);
  d->dn      = g_strdup (sbv_user_get_dn (user));
  d->enabled = enabled;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) set_enabled_data_free);
  g_task_run_in_thread (task, set_enabled_thread);
  g_object_unref (task);
}

gboolean
sbv_users_set_enabled_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Reset password ─────────────────────────────────────────────────────── */

typedef struct {
  char *dn;
  char *new_password;
} ResetPwData;

static void
reset_pw_data_free (ResetPwData *d)
{
  g_free (d->dn);
  if (d->new_password) {
    memset (d->new_password, 0, strlen (d->new_password));
    g_free (d->new_password);
  }
  g_free (d);
}

static void
reset_pw_thread (GTask *task, gpointer source, gpointer task_data,
                 GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  ResetPwData   *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  /* AD stores unicodePwd as UTF-16LE quoted string */
  char *quoted = g_strdup_printf ("\"%s\"", d->new_password);
  gsize utf16_len = 0;
  GError *err = NULL;
  char *utf16 = g_convert (quoted, -1, "UTF-16LE", "UTF-8",
                            NULL, &utf16_len, &err);
  g_free (quoted);

  if (!utf16) {
    sbv_connection_release_ldap (conn);
    g_task_return_error (task, err);
    return;
  }

  struct berval bv = { .bv_val = utf16, .bv_len = utf16_len };
  struct berval *bv_vals[] = { &bv, NULL };

  LDAPMod mod  = { LDAP_MOD_REPLACE | LDAP_MOD_BVALUES, "unicodePwd",
                   { .modv_bvals = bv_vals } };
  LDAPMod *mods[] = { &mod, NULL };

  int rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  g_free (utf16);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Password reset failed: %s\n"
                             "(Hint: requires LDAPS or STARTTLS)",
                             ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_users_reset_password_async (SbvConnection       *conn,
                                 SbvUser             *user,
                                 const char          *new_password,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  ResetPwData *d = g_new0 (ResetPwData, 1);
  d->dn          = g_strdup (sbv_user_get_dn (user));
  d->new_password = g_strdup (new_password);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) reset_pw_data_free);
  g_task_run_in_thread (task, reset_pw_thread);
  g_object_unref (task);
}

gboolean
sbv_users_reset_password_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
