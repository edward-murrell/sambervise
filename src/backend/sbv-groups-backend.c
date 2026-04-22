#include "sbv-groups-backend.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── List groups ────────────────────────────────────────────────────────── */

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
  const char *filter = "(objectClass=group)";
  const char *attrs[] = {
    "distinguishedName", "sAMAccountName", "cn",
    "description", "groupType", "member",
    /* RFC2307 / POSIX */
    "gidNumber", "memberUid",
    NULL
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

  GListStore *store = g_list_store_new (SBV_TYPE_GROUP);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      SbvGroup *group = sbv_group_new ();
      struct berval **bv;

#define GET_STR(attr, setter) \
      bv = ldap_get_values_len (ld, entry, (attr)); \
      if (bv && bv[0]) setter (group, bv[0]->bv_val); \
      ldap_value_free_len (bv);

      GET_STR ("distinguishedName", sbv_group_set_dn)
      GET_STR ("sAMAccountName",    sbv_group_set_sam)
      GET_STR ("cn",                sbv_group_set_display_name)
      GET_STR ("description",       sbv_group_set_description)

#undef GET_STR

      bv = ldap_get_values_len (ld, entry, "groupType");
      if (bv && bv[0])
        sbv_group_set_group_type (group, (gint32) strtol (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

      /* member is a multi-value attribute; copy to GStrv */
      bv = ldap_get_values_len (ld, entry, "member");
      if (bv) {
        int count = ldap_count_values_len (bv);
        char **members = g_new0 (char *, count + 1);
        for (int i = 0; i < count; i++)
          members[i] = g_strndup (bv[i]->bv_val, bv[i]->bv_len);
        sbv_group_set_members (group, members);
        ldap_value_free_len (bv);
      }

      /* RFC2307 / POSIX attributes */
      bv = ldap_get_values_len (ld, entry, "gidNumber");
      if (bv && bv[0])
        sbv_group_set_gid_number (group, (gint) strtol (bv[0]->bv_val, NULL, 10));
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "memberUid");
      if (bv) {
        int count = ldap_count_values_len (bv);
        char **uids = g_new0 (char *, count + 1);
        for (int i = 0; i < count; i++)
          uids[i] = g_strndup (bv[i]->bv_val, bv[i]->bv_len);
        sbv_group_set_member_uid (group, uids);
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
        sbv_group_set_ldap_attrs (group, raw);
      }

      g_list_store_append (store, group);
      g_object_unref (group);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, store, g_object_unref);
}

void
sbv_groups_list_async (SbvConnection       *conn,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

GListStore *
sbv_groups_list_finish (SbvConnection *conn,
                         GAsyncResult  *result,
                         GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Shared member-modify helper ────────────────────────────────────────── */

typedef struct {
  char *group_dn;
  char *member_dn;
  int   ldap_op; /* LDAP_MOD_ADD or LDAP_MOD_DELETE */
} MemberData;

static void
member_data_free (MemberData *d)
{
  g_free (d->group_dn);
  g_free (d->member_dn);
  g_free (d);
}

static void
modify_member_thread (GTask *task, gpointer source, gpointer task_data,
                      GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  MemberData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *vals[]    = { d->member_dn, NULL };
  LDAPMod mod     = { d->ldap_op, "member", { .modv_strvals = vals } };
  LDAPMod *mods[] = { &mod, NULL };

  int rc = ldap_modify_ext_s (ld, d->group_dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Modify failed: %s", ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

/* ── Add member ─────────────────────────────────────────────────────────── */

void
sbv_groups_add_member_async (SbvConnection       *conn,
                              SbvGroup            *group,
                              const char          *member_dn,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  MemberData *d  = g_new0 (MemberData, 1);
  d->group_dn    = g_strdup (sbv_group_get_dn (group));
  d->member_dn   = g_strdup (member_dn);
  d->ldap_op     = LDAP_MOD_ADD;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) member_data_free);
  g_task_run_in_thread (task, modify_member_thread);
  g_object_unref (task);
}

gboolean
sbv_groups_add_member_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Remove member ──────────────────────────────────────────────────────── */

void
sbv_groups_remove_member_async (SbvConnection       *conn,
                                 SbvGroup            *group,
                                 const char          *member_dn,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  MemberData *d = g_new0 (MemberData, 1);
  d->group_dn   = g_strdup (sbv_group_get_dn (group));
  d->member_dn  = g_strdup (member_dn);
  d->ldap_op    = LDAP_MOD_DELETE;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) member_data_free);
  g_task_run_in_thread (task, modify_member_thread);
  g_object_unref (task);
}

gboolean
sbv_groups_remove_member_finish (SbvConnection *conn,
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
  gint           gid_number;
} GroupUnixData;

static void
group_unix_data_free (GroupUnixData *d)
{
  g_free (d->dn);
  g_free (d);
}

static void
set_group_unix_thread (GTask *task, gpointer source, gpointer task_data,
                        GCancellable *cancellable)
{
  (void) cancellable;
  GroupUnixData *d    = task_data;
  SbvConnection *conn = SBV_CONNECTION (source);
  LDAP          *ld   = sbv_connection_acquire_ldap (conn);

  if (!ld) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  LDAPMod  mod;
  LDAPMod *mods[2] = { &mod, NULL };
  char     gid_buf[32];
  char    *vals[2]  = { gid_buf, NULL };

  memset (&mod, 0, sizeof mod);
  mod.mod_op   = LDAP_MOD_REPLACE;
  mod.mod_type = "gidNumber";
  if (d->gid_number >= 0) {
    g_snprintf (gid_buf, sizeof gid_buf, "%d", d->gid_number);
    mod.mod_values = vals;
  }

  int rc = ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Failed to set Unix attributes: %s",
                             ldap_err2string (rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_groups_set_unix_attrs_async (SbvConnection       *conn,
                                  SbvGroup            *group,
                                  gint                 gid_number,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GroupUnixData *d = g_new0 (GroupUnixData, 1);
  d->conn       = conn;
  d->dn         = g_strdup (sbv_group_get_dn (group));
  d->gid_number = gid_number;

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) group_unix_data_free);
  g_task_run_in_thread (task, set_group_unix_thread);
  g_object_unref (task);
}

gboolean
sbv_groups_set_unix_attrs_finish (SbvConnection *conn,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
