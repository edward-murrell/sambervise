#include "sbv-bulk.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <string.h>

#define UAC_ACCOUNTDISABLE 0x0002

/* ── Results ────────────────────────────────────────────────────────────── */

/* Frees one failure record. */
static void
failure_free (SbvBulkFailure *f)
{
  g_free (f->dn);
  g_free (f->message);
  g_free (f);
}

/* Starts an empty result for `total` objects. */
static SbvBulkResult *
result_new (guint total)
{
  SbvBulkResult *r = g_new0 (SbvBulkResult, 1);
  r->total    = total;
  r->failures = g_ptr_array_new_with_free_func ((GDestroyNotify) failure_free);
  return r;
}

/* Frees a result and its failure records. */
void
sbv_bulk_result_free (SbvBulkResult *result)
{
  if (!result) return;
  g_ptr_array_unref (result->failures);
  g_free (result);
}

/* Records a failure for `dn`. Takes ownership of `message`. */
static void
result_fail (SbvBulkResult *r, const char *dn, char *message)
{
  SbvBulkFailure *f = g_new0 (SbvBulkFailure, 1);
  f->dn      = g_strdup (dn);
  f->message = message;
  g_ptr_array_add (r->failures, f);
}

/* Turns an LDAP result code into a short message. Adds the DC's
 * diagnostic text when there is one, since it often names the rule that
 * failed, such as a password policy. Caller frees. */
static char *
ldap_message (LDAP *ld, int rc)
{
  const char *hint = NULL;
  switch (rc) {
    case LDAP_INSUFFICIENT_ACCESS:
      hint = "Access denied.";
      break;
    case LDAP_NO_SUCH_OBJECT:
      hint = "The object no longer exists.";
      break;
    case LDAP_ALREADY_EXISTS:
      hint = "An object with that name already exists there.";
      break;
    case LDAP_TYPE_OR_VALUE_EXISTS:
      hint = "Already a member.";
      break;
    case LDAP_NOT_ALLOWED_ON_NONLEAF:
      hint = "It has child objects. Delete those first.";
      break;
    case LDAP_CONSTRAINT_VIOLATION:
      hint = "The DC rejected the value. A password may not meet the policy.";
      break;
    default:
      break;
  }

  char *diag = NULL;
  ldap_get_option (ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag);
  char *msg = (diag && *diag)
    ? g_strdup_printf ("%s (%s)", hint ? hint : ldap_err2string (rc), diag)
    : g_strdup (hint ? hint : ldap_err2string (rc));
  if (diag) ldap_memfree (diag);
  return msg;
}

/* ── Per-object operations (caller holds the LDAP lock) ─────────────────── */

/* Sets or clears ACCOUNTDISABLE, keeping every other UAC bit. Returns
 * an LDAP result code. */
static int
op_set_enabled (LDAP *ld, const char *dn, gboolean enabled)
{
  char          *attrs[] = { (char *) "userAccountControl", NULL };
  struct timeval tv      = { 10, 0 };
  LDAPMessage   *res     = NULL;

  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, &tv, 1, &res);
  if (rc != LDAP_SUCCESS) {
    if (res) ldap_msgfree (res);
    return rc;
  }

  gint64          uac   = 0;
  LDAPMessage    *entry = ldap_first_entry (ld, res);
  struct berval **bv    = entry ? ldap_get_values_len (ld, entry, "userAccountControl") : NULL;
  if (bv && bv[0]) uac = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
  ldap_value_free_len (bv);
  ldap_msgfree (res);
  if (!entry) return LDAP_NO_SUCH_OBJECT;

  gint64 new_uac = enabled ? (uac & ~(gint64) UAC_ACCOUNTDISABLE)
                           : (uac | UAC_ACCOUNTDISABLE);
  if (new_uac == uac) return LDAP_SUCCESS;

  char    uac_str[32];
  g_snprintf (uac_str, sizeof uac_str, "%" G_GINT64_FORMAT, new_uac);
  char   *vals[] = { uac_str, NULL };
  LDAPMod mod    = { LDAP_MOD_REPLACE, (char *) "userAccountControl",
                     { .modv_strvals = vals } };
  LDAPMod *mods[] = { &mod, NULL };
  return ldap_modify_ext_s (ld, dn, mods, NULL, NULL);
}

/* Replaces unicodePwd, then sets pwdLastSet to 0 when the user must
 * change it. `utf16` is the quoted password in UTF-16LE. */
static int
op_reset_password (LDAP *ld, const char *dn, const char *utf16, gsize len,
                   gboolean must_change)
{
  struct berval  bv       = { .bv_val = (char *) utf16, .bv_len = len };
  struct berval *bvals[]  = { &bv, NULL };
  LDAPMod        pw_mod   = { LDAP_MOD_REPLACE | LDAP_MOD_BVALUES,
                              (char *) "unicodePwd", { .modv_bvals = bvals } };
  LDAPMod       *pw_mods[] = { &pw_mod, NULL };

  int rc = ldap_modify_ext_s (ld, dn, pw_mods, NULL, NULL);
  if (rc != LDAP_SUCCESS || !must_change) return rc;

  char   *zero[]   = { (char *) "0", NULL };
  LDAPMod pls_mod  = { LDAP_MOD_REPLACE, (char *) "pwdLastSet",
                       { .modv_strvals = zero } };
  LDAPMod *mods[]  = { &pls_mod, NULL };
  return ldap_modify_ext_s (ld, dn, mods, NULL, NULL);
}

/* Moves `dn` under `parent`, keeping its RDN. An object already in
 * `parent` is left alone. */
static int
op_move (LDAP *ld, const char *dn, const char *parent)
{
  char *rdn = NULL, *cur = NULL;
  if (!sbv_dn_split (dn, &rdn, &cur)) return LDAP_INVALID_DN_SYNTAX;

  int rc = LDAP_SUCCESS;
  if (!sbv_dn_equal (cur, parent))
    rc = ldap_rename_s (ld, dn, rdn, parent, 1, NULL, NULL);
  g_free (rdn);
  g_free (cur);
  return rc;
}

/* Adds `dn` to the group at `group_dn`. An object already in the group
 * counts as success. */
static int
op_add_to_group (LDAP *ld, const char *dn, const char *group_dn)
{
  char   *vals[] = { (char *) dn, NULL };
  LDAPMod mod    = { LDAP_MOD_ADD, (char *) "member", { .modv_strvals = vals } };
  LDAPMod *mods[] = { &mod, NULL };

  int rc = ldap_modify_ext_s (ld, group_dn, mods, NULL, NULL);
  if (rc == LDAP_TYPE_OR_VALUE_EXISTS || rc == LDAP_ALREADY_EXISTS)
    rc = LDAP_SUCCESS;
  return rc;
}

/* Deletes the leaf object at `dn`. Objects with children fail with
 * LDAP_NOT_ALLOWED_ON_NONLEAF. Critical system objects, such as
 * Administrator, krbtgt and DC accounts, are refused with `*refused` set
 * to a message for the user. */
static int
op_delete (LDAP *ld, const char *dn, char **refused)
{
  char          *attrs[] = { (char *) "isCriticalSystemObject", NULL };
  struct timeval tv      = { 10, 0 };
  LDAPMessage   *res     = NULL;

  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, &tv, 1, &res);
  if (rc != LDAP_SUCCESS) {
    if (res) ldap_msgfree (res);
    return rc;
  }

  LDAPMessage    *entry    = ldap_first_entry (ld, res);
  struct berval **bv       = entry ? ldap_get_values_len (ld, entry, "isCriticalSystemObject") : NULL;
  gboolean        critical = bv && bv[0]
                             && g_ascii_strncasecmp (bv[0]->bv_val, "TRUE", bv[0]->bv_len) == 0;
  ldap_value_free_len (bv);
  ldap_msgfree (res);
  if (!entry) return LDAP_NO_SUCH_OBJECT;

  if (critical) {
    *refused = g_strdup ("Skipped. It's a critical system object.");
    return LDAP_UNWILLING_TO_PERFORM;
  }
  return ldap_delete_ext_s (ld, dn, NULL, NULL);
}

/* ── Bulk run ───────────────────────────────────────────────────────────── */

typedef struct {
  SbvBulkAction action;
  char        **dns;
  char         *arg;
  gboolean      must_change;
} RunData;

/* Frees a run payload, wiping a password argument first. */
static void
run_data_free (RunData *d)
{
  g_strfreev (d->dns);
  if (d->arg) {
    memset (d->arg, 0, strlen (d->arg));
    g_free (d->arg);
  }
  g_free (d);
}

/* Worker: holds the LDAP lock and applies the action to each DN in turn.
 * Runs on a GTask thread. */
static void
run_thread (GTask *task, gpointer source, gpointer task_data,
            GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  RunData       *d    = task_data;
  (void) cancellable;

  /* Encode the password once, before taking the lock. */
  char  *utf16     = NULL;
  gsize  utf16_len = 0;
  if (d->action == SBV_BULK_RESET_PASSWORD) {
    char   *quoted = g_strdup_printf ("\"%s\"", d->arg ? d->arg : "");
    GError *err    = NULL;
    utf16 = g_convert (quoted, -1, "UTF-16LE", "UTF-8", NULL, &utf16_len, &err);
    memset (quoted, 0, strlen (quoted));
    g_free (quoted);
    if (!utf16) {
      g_task_return_error (task, err);
      return;
    }
  }

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_free (utf16);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  SbvBulkResult *r = result_new (g_strv_length (d->dns));
  for (char **dn = d->dns; *dn; dn++) {
    int   rc      = LDAP_OTHER;
    char *refused = NULL;
    switch (d->action) {
      case SBV_BULK_ENABLE:
        rc = op_set_enabled (ld, *dn, TRUE);
        break;
      case SBV_BULK_DISABLE:
        rc = op_set_enabled (ld, *dn, FALSE);
        break;
      case SBV_BULK_RESET_PASSWORD:
        rc = op_reset_password (ld, *dn, utf16, utf16_len, d->must_change);
        break;
      case SBV_BULK_MOVE:
        rc = op_move (ld, *dn, d->arg);
        break;
      case SBV_BULK_ADD_TO_GROUP:
        rc = op_add_to_group (ld, *dn, d->arg);
        break;
      case SBV_BULK_DELETE:
        rc = op_delete (ld, *dn, &refused);
        break;
    }
    if (rc == LDAP_SUCCESS) r->succeeded++;
    else                    result_fail (r, *dn, refused ? refused : ldap_message (ld, rc));
  }
  sbv_connection_release_ldap (conn);

  if (utf16) {
    memset (utf16, 0, utf16_len);
    g_free (utf16);
  }
  g_task_return_pointer (task, r, (GDestroyNotify) sbv_bulk_result_free);
}

void
sbv_bulk_run_async (SbvConnection       *conn,
                    SbvBulkAction        action,
                    const char * const  *dns,
                    const char          *arg,
                    gboolean             must_change,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  if ((action == SBV_BULK_MOVE || action == SBV_BULK_RESET_PASSWORD
       || action == SBV_BULK_ADD_TO_GROUP)
      && (!arg || !*arg)) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             action == SBV_BULK_MOVE ? "Pick a destination first."
                             : action == SBV_BULK_ADD_TO_GROUP ? "Pick a group first."
                             : "Enter a password first.");
    g_object_unref (task);
    return;
  }

  RunData *d     = g_new0 (RunData, 1);
  d->action      = action;
  d->dns         = g_strdupv ((char **) dns);
  d->arg         = g_strdup (arg);
  d->must_change = must_change;
  if (!d->dns) d->dns = g_new0 (char *, 1);

  g_task_set_task_data (task, d, (GDestroyNotify) run_data_free);
  g_task_run_in_thread (task, run_thread);
  g_object_unref (task);
}

SbvBulkResult *
sbv_bulk_run_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Apply user template ────────────────────────────────────────────────── */

typedef struct {
  char   *new_dn;
  char   *home_directory;
  char   *home_drive;
  char   *script_path;
  char   *profile_path;
  gint64  template_uac;
  gint64  account_expires;
  char  **groups;
} TemplateData;

/* Frees a template payload. */
static void
template_data_free (TemplateData *d)
{
  g_free (d->new_dn);
  g_free (d->home_directory);
  g_free (d->home_drive);
  g_free (d->script_path);
  g_free (d->profile_path);
  g_strfreev (d->groups);
  g_free (d);
}

/* Adds a REPLACE for `attr` to `mods` when `value` is set. `vals` must
 * have room for two pointers and live as long as `mods`. */
static void
add_replace (GPtrArray *mods, LDAPMod *mod, char **vals,
             const char *attr, const char *value)
{
  if (!value || !*value) return;
  vals[0] = (char *) value;
  vals[1] = NULL;
  mod->mod_op     = LDAP_MOD_REPLACE;
  mod->mod_type   = (char *) attr;
  mod->mod_values = vals;
  g_ptr_array_add (mods, mod);
}

/* Writes account options, expiry and profile paths onto the new user in
 * one modify. Returns an LDAP result code. */
static int
apply_settings (LDAP *ld, TemplateData *d)
{
  char          *attrs[] = { (char *) "userAccountControl", NULL };
  struct timeval tv      = { 10, 0 };
  LDAPMessage   *res     = NULL;

  int rc = ldap_search_ext_s (ld, d->new_dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, &tv, 1, &res);
  if (rc != LDAP_SUCCESS) {
    if (res) ldap_msgfree (res);
    return rc;
  }
  gint64          uac   = 0;
  LDAPMessage    *entry = ldap_first_entry (ld, res);
  struct berval **bv    = entry ? ldap_get_values_len (ld, entry, "userAccountControl") : NULL;
  if (bv && bv[0]) uac = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
  ldap_value_free_len (bv);
  ldap_msgfree (res);
  if (!entry) return LDAP_NO_SUCH_OBJECT;

  char uac_str[32], exp_str[32];
  g_snprintf (uac_str, sizeof uac_str, "%" G_GINT64_FORMAT,
              sbv_user_template_uac (uac, d->template_uac));
  g_snprintf (exp_str, sizeof exp_str, "%" G_GINT64_FORMAT,
              d->account_expires == G_MAXINT64 ? (gint64) 0 : d->account_expires);

  LDAPMod    m[6];
  char      *v[6][2];
  GPtrArray *mods = g_ptr_array_new ();
  add_replace (mods, &m[0], v[0], "userAccountControl", uac_str);
  if (d->account_expires != G_MAXINT64)
    add_replace (mods, &m[1], v[1], "accountExpires", exp_str);
  add_replace (mods, &m[2], v[2], "homeDirectory", d->home_directory);
  add_replace (mods, &m[3], v[3], "homeDrive",     d->home_drive);
  add_replace (mods, &m[4], v[4], "scriptPath",    d->script_path);
  add_replace (mods, &m[5], v[5], "profilePath",   d->profile_path);
  g_ptr_array_add (mods, NULL);

  rc = ldap_modify_ext_s (ld, d->new_dn, (LDAPMod **) mods->pdata, NULL, NULL);
  g_ptr_array_free (mods, TRUE);
  return rc;
}

/* Adds `member_dn` to the group at `group_dn`. */
static int
add_to_group (LDAP *ld, const char *group_dn, const char *member_dn)
{
  char   *vals[] = { (char *) member_dn, NULL };
  LDAPMod mod    = { LDAP_MOD_ADD, (char *) "member", { .modv_strvals = vals } };
  LDAPMod *mods[] = { &mod, NULL };
  return ldap_modify_ext_s (ld, group_dn, mods, NULL, NULL);
}

/* Worker: applies the settings, then joins each group. Runs on a GTask
 * thread. */
static void
template_thread (GTask *task, gpointer source, gpointer task_data,
                 GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  TemplateData  *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  SbvBulkResult *r = result_new (1 + g_strv_length (d->groups));

  int rc = apply_settings (ld, d);
  if (rc == LDAP_SUCCESS) {
    r->succeeded++;
  } else {
    char *why = ldap_message (ld, rc);
    result_fail (r, d->new_dn,
                 g_strdup_printf ("Account options and profile paths: %s", why));
    g_free (why);
  }

  for (char **g = d->groups; *g; g++) {
    rc = add_to_group (ld, *g, d->new_dn);
    if (rc == LDAP_SUCCESS || rc == LDAP_TYPE_OR_VALUE_EXISTS) r->succeeded++;
    else result_fail (r, *g, ldap_message (ld, rc));
  }
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, r, (GDestroyNotify) sbv_bulk_result_free);
}

void
sbv_bulk_apply_user_template_async (SbvConnection       *conn,
                                    const char          *new_dn,
                                    const char          *new_sam,
                                    SbvUser             *template_user,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  const char *tsam = sbv_user_get_sam (template_user);

  TemplateData *d    = g_new0 (TemplateData, 1);
  d->new_dn          = g_strdup (new_dn);
  d->home_directory  = sbv_user_template_path (sbv_user_get_home_directory (template_user), tsam, new_sam);
  d->home_drive      = g_strdup (sbv_user_get_home_drive (template_user));
  d->script_path     = sbv_user_template_path (sbv_user_get_script_path (template_user), tsam, new_sam);
  d->profile_path    = sbv_user_template_path (sbv_user_get_profile_path (template_user), tsam, new_sam);
  d->template_uac    = sbv_user_get_uac (template_user);
  d->account_expires = sbv_user_get_account_expires (template_user);
  d->groups          = g_strdupv ((char **) sbv_user_get_member_of (template_user));
  if (!d->groups) d->groups = g_new0 (char *, 1);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) template_data_free);
  g_task_run_in_thread (task, template_thread);
  g_object_unref (task);
}

SbvBulkResult *
sbv_bulk_apply_user_template_finish (SbvConnection *conn,
                                     GAsyncResult  *result,
                                     GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}
