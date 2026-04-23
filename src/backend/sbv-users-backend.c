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
        sbv_user_set_ldap_attrs (user, raw);
      }

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

/* ── Set Unix / RFC2307 attributes ─────────────────────────────────────── */

typedef struct {
  SbvConnection *conn;
  char          *dn;
  gint           uid_number;
  gint           gid_number;
  char          *login_shell;
  char          *home_dir;
  char          *gecos;
} UnixAttrsData;

static void
unix_attrs_data_free (UnixAttrsData *d)
{
  g_free (d->dn);
  g_free (d->login_shell);
  g_free (d->home_dir);
  g_free (d->gecos);
  g_free (d);
}

static void
set_unix_attrs_thread (GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
  (void) cancellable;
  UnixAttrsData *d    = task_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  LDAP          *ld   = sbv_connection_acquire_ldap (conn);

  if (!ld) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

#define N_UNIX_MODS 5
  LDAPMod  mods[N_UNIX_MODS];
  LDAPMod *modp[N_UNIX_MODS + 1];
  char    *vslots[N_UNIX_MODS][2];
  char     uid_buf[32], gid_buf[32];
  int      i = 0;

  memset (mods, 0, sizeof mods);

  /* uidNumber */
  mods[i].mod_op = LDAP_MOD_REPLACE; mods[i].mod_type = "uidNumber";
  if (d->uid_number >= 0) {
    g_snprintf (uid_buf, sizeof uid_buf, "%d", d->uid_number);
    vslots[i][0] = uid_buf; vslots[i][1] = NULL;
    mods[i].mod_values = vslots[i];
  }
  modp[i] = &mods[i]; i++;

  /* gidNumber */
  mods[i].mod_op = LDAP_MOD_REPLACE; mods[i].mod_type = "gidNumber";
  if (d->gid_number >= 0) {
    g_snprintf (gid_buf, sizeof gid_buf, "%d", d->gid_number);
    vslots[i][0] = gid_buf; vslots[i][1] = NULL;
    mods[i].mod_values = vslots[i];
  }
  modp[i] = &mods[i]; i++;

#define STR_MOD(attr, field) \
  mods[i].mod_op = LDAP_MOD_REPLACE; mods[i].mod_type = (attr); \
  if ((field) && *(field)) { \
    vslots[i][0] = (char *)(field); vslots[i][1] = NULL; \
    mods[i].mod_values = vslots[i]; \
  } \
  modp[i] = &mods[i]; i++;

  STR_MOD ("loginShell",    d->login_shell)
  STR_MOD ("homeDirectory", d->home_dir)
  STR_MOD ("gecos",         d->gecos)
#undef STR_MOD
#undef N_UNIX_MODS

  modp[i] = NULL;

  int rc = ldap_modify_ext_s (ld, d->dn, modp, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Failed to set Unix attributes: %s",
                             ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_users_set_unix_attrs_async (SbvConnection       *conn,
                                 SbvUser             *user,
                                 gint                 uid_number,
                                 gint                 gid_number,
                                 const char          *login_shell,
                                 const char          *home_dir,
                                 const char          *gecos,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  UnixAttrsData *d = g_new0 (UnixAttrsData, 1);
  d->conn        = conn;
  d->dn          = g_strdup (sbv_user_get_dn (user));
  d->uid_number  = uid_number;
  d->gid_number  = gid_number;
  d->login_shell = g_strdup (login_shell);
  d->home_dir    = g_strdup (home_dir);
  d->gecos       = g_strdup (gecos);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) unix_attrs_data_free);
  g_task_run_in_thread (task, set_unix_attrs_thread);
  g_object_unref (task);
}

gboolean
sbv_users_set_unix_attrs_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Create user ────────────────────────────────────────────────────────── */

typedef struct {
  char *sam;
  char *cn;
  char *given_name;
  char *sn;
  char *container_dn;
} CreateUserData;

/* Frees a CreateUserData struct previously passed via g_task_set_task_data. */
static void
create_user_data_free (CreateUserData *d)
{
  g_free (d->sam);
  g_free (d->cn);
  g_free (d->given_name);
  g_free (d->sn);
  g_free (d->container_dn);
  g_free (d);
}

/* Worker thread: builds the LDAP entry and issues ldap_add_ext_s for a new
 * disabled user account. Created with userAccountControl = NORMAL_ACCOUNT |
 * ACCOUNTDISABLE so AD accepts the entry without requiring a password set in
 * the same operation (which would need LDAPS/STARTTLS). */
static void
create_user_thread (GTask *task, gpointer source, gpointer task_data,
                    GCancellable *cancellable)
{
  SbvConnection  *conn = SBV_CONNECTION (source);
  CreateUserData *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *dn = g_strdup_printf ("CN=%s,%s", d->cn, d->container_dn);

  /* Build attribute set. AD requires sAMAccountName and userAccountControl
   * for user objects; objectClass list mirrors what AD assigns by default. */
  char *oc_vals[]   = { "top", "person", "organizationalPerson", "user", NULL };
  char *sam_vals[]  = { d->sam, NULL };
  char  uac_buf[16];
  g_snprintf (uac_buf, sizeof uac_buf, "%d",
              UAC_NORMAL_ACCOUNT | UAC_ACCOUNTDISABLE);
  char *uac_vals[]  = { uac_buf, NULL };
  char *cn_vals[]   = { d->cn, NULL };
  char *gn_vals[]   = { d->given_name, NULL };
  char *sn_vals[]   = { d->sn, NULL };

  LDAPMod  oc_mod   = { LDAP_MOD_ADD, "objectClass",        { .modv_strvals = oc_vals  } };
  LDAPMod  sam_mod  = { LDAP_MOD_ADD, "sAMAccountName",     { .modv_strvals = sam_vals } };
  LDAPMod  uac_mod  = { LDAP_MOD_ADD, "userAccountControl", { .modv_strvals = uac_vals } };
  LDAPMod  cn_mod   = { LDAP_MOD_ADD, "cn",                 { .modv_strvals = cn_vals  } };
  LDAPMod  gn_mod   = { LDAP_MOD_ADD, "givenName",          { .modv_strvals = gn_vals  } };
  LDAPMod  sn_mod   = { LDAP_MOD_ADD, "sn",                 { .modv_strvals = sn_vals  } };

  LDAPMod *mods[8];
  int      n = 0;
  mods[n++] = &oc_mod;
  mods[n++] = &cn_mod;
  mods[n++] = &sam_mod;
  mods[n++] = &uac_mod;
  if (d->given_name && *d->given_name) mods[n++] = &gn_mod;
  if (d->sn         && *d->sn)         mods[n++] = &sn_mod;
  mods[n] = NULL;

  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_free (dn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Create failed: %s", ldap_err2string (rc));
    return;
  }

  g_task_return_pointer (task, dn, g_free);
}

/* Public entry point — see header. Validates required fields and dispatches
 * to a worker thread. */
void
sbv_users_create_async (SbvConnection       *conn,
                         const char          *sam,
                         const char          *cn,
                         const char          *given_name,
                         const char          *sn,
                         const char          *container_dn,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  if (!sam || !*sam || !cn || !*cn || !container_dn || !*container_dn) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Username, full name and container are required");
    g_object_unref (task);
    return;
  }

  CreateUserData *d = g_new0 (CreateUserData, 1);
  d->sam          = g_strdup (sam);
  d->cn           = g_strdup (cn);
  d->given_name   = g_strdup (given_name);
  d->sn           = g_strdup (sn);
  d->container_dn = g_strdup (container_dn);

  g_task_set_task_data (task, d, (GDestroyNotify) create_user_data_free);
  g_task_run_in_thread (task, create_user_thread);
  g_object_unref (task);
}

/* Returns the DN of the newly-created user (caller frees) or NULL on error. */
char *
sbv_users_create_finish (SbvConnection *conn,
                          GAsyncResult  *result,
                          GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Delete user ────────────────────────────────────────────────────────── */

/* Worker thread: issues a single ldap_delete_ext_s call. AD users normally
 * have no children, so leaf delete is sufficient. */
static void
delete_user_thread (GTask *task, gpointer source, gpointer task_data,
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

/* Public entry — schedules the delete on a worker thread. The user's DN is
 * captured at call time so the SbvUser may be freed before completion. */
void
sbv_users_delete_async (SbvConnection       *conn,
                         SbvUser             *user,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (sbv_user_get_dn (user)), g_free);
  g_task_run_in_thread (task, delete_user_thread);
  g_object_unref (task);
}

/* Reports success/failure of a previously-scheduled delete. */
gboolean
sbv_users_delete_finish (SbvConnection *conn,
                          GAsyncResult  *result,
                          GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
