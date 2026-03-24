#include "sbv-users-backend.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

#define UAC_ACCOUNTDISABLE     0x0002
#define UAC_NORMAL_ACCOUNT     0x0200
#define UAC_DONT_EXPIRE_PASSWD 0x10000

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

  const char *base   = sbv_connection_get_base_dn (conn);
  const char *filter = "(&(objectClass=user)(!(objectClass=computer)))";
  const char *attrs[] = {
    "distinguishedName", "sAMAccountName",
    "cn", "displayName", "givenName", "sn",
    "mail", "description", "userAccountControl",
    "pwdLastSet", "accountExpires",
    /* RFC2307 / POSIX */
    "uidNumber", "gidNumber", "loginShell", "homeDirectory", "gecos",
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
      GET_STR ("cn",                sbv_user_set_cn)
      GET_STR ("displayName",       sbv_user_set_display_name)
      GET_STR ("givenName",         sbv_user_set_given_name)
      GET_STR ("sn",                sbv_user_set_sn)
      GET_STR ("mail",              sbv_user_set_email)
      GET_STR ("description",       sbv_user_set_description)

#undef GET_STR

      bv = ldap_get_values_len (ld, entry, "userAccountControl");
      if (bv && bv[0]) {
        gint64 uac = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
        sbv_user_set_uac (user, uac);
        sbv_user_set_enabled (user, !(uac & UAC_ACCOUNTDISABLE));
      }
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "pwdLastSet");
      if (bv && bv[0])
        sbv_user_set_pwd_last_set (user, g_ascii_strtoll (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "accountExpires");
      if (bv && bv[0]) {
        gint64 expires = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
        /* 0 in AD also means "never" for accountExpires in some contexts */
        sbv_user_set_account_expires (user, (expires == 0) ? G_MAXINT64 : expires);
      }
      ldap_value_free_len (bv);

      /* RFC2307 / POSIX attributes */
      bv = ldap_get_values_len (ld, entry, "uidNumber");
      if (bv && bv[0])
        sbv_user_set_uid_number (user, (gint) strtol (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "gidNumber");
      if (bv && bv[0])
        sbv_user_set_gid_number (user, (gint) strtol (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

#define GET_STR_U(attr, setter) \
      bv = ldap_get_values_len (ld, entry, (attr)); \
      if (bv && bv[0]) setter (user, bv[0]->bv_val); \
      ldap_value_free_len (bv);

      GET_STR_U ("loginShell",     sbv_user_set_login_shell)
      GET_STR_U ("homeDirectory",  sbv_user_set_home_dir)
      GET_STR_U ("gecos",          sbv_user_set_gecos)

#undef GET_STR_U

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

  const char    *attrs[] = { "userAccountControl", NULL };
  LDAPMessage   *result  = NULL;
  struct timeval  tv     = { 10, 0 };

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

  LDAPMessage  *entry = ldap_first_entry (ld, result);
  long          uac   = UAC_NORMAL_ACCOUNT;
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

  char   *quoted   = g_strdup_printf ("\"%s\"", d->new_password);
  gsize   utf16_len = 0;
  GError *err      = NULL;
  char   *utf16    = g_convert (quoted, -1, "UTF-16LE", "UTF-8",
                                NULL, &utf16_len, &err);
  g_free (quoted);

  if (!utf16) {
    sbv_connection_release_ldap (conn);
    g_task_return_error (task, err);
    return;
  }

  struct berval  bv      = { .bv_val = utf16, .bv_len = utf16_len };
  struct berval *bv_vals[] = { &bv, NULL };
  LDAPMod        mod     = { LDAP_MOD_REPLACE | LDAP_MOD_BVALUES, "unicodePwd",
                             { .modv_bvals = bv_vals } };
  LDAPMod       *mods[]  = { &mod, NULL };

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
  ResetPwData *d  = g_new0 (ResetPwData, 1);
  d->dn           = g_strdup (sbv_user_get_dn (user));
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

/* ── Update attributes ──────────────────────────────────────────────────── */

typedef struct {
  char *dn;
  char *cn;
  char *display_name;
  char *given_name;
  char *sn;
  char *email;
} UpdateAttrsData;

static void
update_attrs_data_free (UpdateAttrsData *d)
{
  g_free (d->dn);
  g_free (d->cn);
  g_free (d->display_name);
  g_free (d->given_name);
  g_free (d->sn);
  g_free (d->email);
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

  /* cn is the RDN of user objects and cannot be changed via ldap_modify.
   * It requires ldap_rename_s. Do that first and track the resulting DN. */
  char *effective_dn = g_strdup (d->dn);

  if (d->cn && *d->cn) {
    char       *new_rdn  = g_strdup_printf ("CN=%s", d->cn);
    const char *comma    = strchr (d->dn, ',');
    const char *parent   = comma ? comma + 1 : "";

    int rc = ldap_rename_s (ld, d->dn, new_rdn, NULL, 1, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      g_free (new_rdn);
      g_free (effective_dn);
      sbv_connection_release_ldap (conn);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                               "Rename failed: %s", ldap_err2string (rc));
      return;
    }
    g_free (effective_dn);
    effective_dn = g_strdup_printf ("CN=%s,%s", d->cn, parent);
    g_free (new_rdn);
  }

  /* Modify remaining attributes (cn excluded — it is the RDN). */
  LDAPMod  mods_buf[4];
  LDAPMod *mods[5];
  int      n = 0;

  char *display_vals[] = { d->display_name, NULL };
  char *given_vals[]   = { d->given_name,   NULL };
  char *sn_vals[]      = { d->sn,           NULL };
  char *email_vals[]   = { d->email,        NULL };

#define ADD_MOD(attr, val, vals_arr) \
  if ((val) != NULL) { \
    mods_buf[n] = (LDAPMod){ LDAP_MOD_REPLACE, (char *)(attr), \
                              { .modv_strvals = (*(val)) ? (vals_arr) : NULL } }; \
    mods[n] = &mods_buf[n]; \
    n++; \
  }

  ADD_MOD ("displayName", d->display_name, display_vals)
  ADD_MOD ("givenName",   d->given_name,   given_vals)
  ADD_MOD ("sn",          d->sn,           sn_vals)
  ADD_MOD ("mail",        d->email,        email_vals)

#undef ADD_MOD

  mods[n] = NULL;

  int rc = (n > 0) ? ldap_modify_ext_s (ld, effective_dn, mods, NULL, NULL)
                   : LDAP_SUCCESS;
  g_free (effective_dn);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Attribute update failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_users_update_attrs_async (SbvConnection       *conn,
                               SbvUser             *user,
                               const char          *cn,
                               const char          *display_name,
                               const char          *given_name,
                               const char          *sn,
                               const char          *email,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  UpdateAttrsData *d = g_new0 (UpdateAttrsData, 1);
  d->dn           = g_strdup (sbv_user_get_dn (user));
  d->cn           = g_strdup (cn);
  d->display_name = g_strdup (display_name);
  d->given_name   = g_strdup (given_name);
  d->sn           = g_strdup (sn);
  d->email        = g_strdup (email);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) update_attrs_data_free);
  g_task_run_in_thread (task, update_attrs_thread);
  g_object_unref (task);
}

gboolean
sbv_users_update_attrs_finish (SbvConnection *conn,
                                GAsyncResult  *result,
                                GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Set password flags ─────────────────────────────────────────────────── */

typedef struct {
  char    *dn;
  gint64   current_uac;
  gboolean force_change;
  gboolean never_expires;
  gint64   account_expires;
} SetPwFlagsData;

static void
set_pw_flags_data_free (SetPwFlagsData *d)
{
  g_free (d->dn);
  g_free (d);
}

static void
set_pw_flags_thread (GTask *task, gpointer source, gpointer task_data,
                     GCancellable *cancellable)
{
  SbvConnection  *conn = SBV_CONNECTION (source);
  SetPwFlagsData *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  /* Toggle the DONT_EXPIRE_PASSWD flag */
  gint64 new_uac = d->current_uac;
  if (d->never_expires)
    new_uac |= UAC_DONT_EXPIRE_PASSWD;
  else
    new_uac &= ~(gint64) UAC_DONT_EXPIRE_PASSWD;

  char uac_str[32], expires_str[32];
  snprintf (uac_str,     sizeof uac_str,     "%" G_GINT64_FORMAT, new_uac);
  snprintf (expires_str, sizeof expires_str, "%" G_GINT64_FORMAT, d->account_expires);

  /* pwdLastSet: "0" = must change; "-1" = set to current time (clears force) */
  char *pwd_str = d->force_change ? "0" : "-1";

  char *uac_vals[]     = { uac_str,     NULL };
  char *pwd_vals[]     = { pwd_str,     NULL };
  char *expire_vals[]  = { expires_str, NULL };

  LDAPMod uac_mod     = { LDAP_MOD_REPLACE, "userAccountControl",
                          { .modv_strvals = uac_vals } };
  LDAPMod pwd_mod     = { LDAP_MOD_REPLACE, "pwdLastSet",
                          { .modv_strvals = pwd_vals } };
  LDAPMod expires_mod = { LDAP_MOD_REPLACE, "accountExpires",
                          { .modv_strvals = expire_vals } };
  LDAPMod *mods[]     = { &uac_mod, &pwd_mod, &expires_mod, NULL };

  int rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Policy update failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_users_set_password_flags_async (SbvConnection       *conn,
                                     SbvUser             *user,
                                     gboolean             force_change,
                                     gboolean             never_expires,
                                     gint64               account_expires,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  SetPwFlagsData *d   = g_new0 (SetPwFlagsData, 1);
  d->dn               = g_strdup (sbv_user_get_dn (user));
  d->current_uac      = sbv_user_get_uac (user);
  d->force_change     = force_change;
  d->never_expires    = never_expires;
  d->account_expires  = account_expires;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) set_pw_flags_data_free);
  g_task_run_in_thread (task, set_pw_flags_thread);
  g_object_unref (task);
}

gboolean
sbv_users_set_password_flags_finish (SbvConnection *conn,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
