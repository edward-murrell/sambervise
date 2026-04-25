#include "sbv-ldap-browse.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── Shared helpers ────────────────────────────────────────────────────── */

/* Reads every attribute of one LDAP entry into a fresh
 * GHashTable<char*, GStrv>. Mirrors the raw-attrs code in users/groups
 * backends. Caller frees the table. */
static GHashTable *
collect_attrs (LDAP *ld, LDAPMessage *entry)
{
  GHashTable *raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free,
                                            (GDestroyNotify) g_strfreev);
  BerElement *ber = NULL;
  for (char *aname = ldap_first_attribute (ld, entry, &ber);
       aname != NULL;
       aname = ldap_next_attribute (ld, entry, ber))
    {
      struct berval **av = ldap_get_values_len (ld, entry, aname);
      if (av) {
        int    cnt = ldap_count_values_len (av);
        char **sv  = g_new0 (char *, cnt + 1);
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
  if (ber) ber_free (ber, 0);
  return raw;
}

/* Returns the leftmost RDN value of `dn`, e.g. "CN=Users,DC=example,..." →
 * "Users". Caller frees. Falls back to the full DN if it can't be parsed. */
static char *
extract_label (const char *dn)
{
  if (!dn || !*dn) return g_strdup ("");
  const char *eq = strchr (dn, '=');
  if (!eq) return g_strdup (dn);
  const char *start = eq + 1;
  const char *comma = strchr (start, ',');
  return comma ? g_strndup (start, comma - start) : g_strdup (start);
}

/* GCompareDataFunc: case-insensitive label comparator for SbvLdapNode so
 * the browser presents children alphabetically rather than in DC return
 * order. */
static int
compare_nodes_by_label (gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void) user_data;
  const char *la = sbv_ldap_node_get_label ((SbvLdapNode *) a);
  const char *lb = sbv_ldap_node_get_label ((SbvLdapNode *) b);
  return g_utf8_collate (la ?: "", lb ?: "");
}

/* ── Children (one-level) ──────────────────────────────────────────────── */

/* Worker: ONELEVEL search returning child entries with all their
 * attributes. The LDAP server is asked for "*" (all user attrs) plus
 * "+" (operational attrs); we settle for "*" so the result matches what
 * the existing detail panes show elsewhere. */
static void
children_thread (GTask *task, gpointer source, gpointer task_data,
                 GCancellable *cancellable)
{
  SbvConnection *conn      = SBV_CONNECTION (source);
  const char    *parent_dn = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 30, 0 };

  int rc = ldap_search_ext_s (ld, parent_dn, LDAP_SCOPE_ONELEVEL,
                               "(objectClass=*)", NULL, 0,
                               NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Browse failed: %s", ldap_err2string (rc));
    return;
  }

  GListStore *store = g_list_store_new (SBV_TYPE_LDAP_NODE);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      char *child_dn = ldap_get_dn (ld, entry);
      if (!child_dn) continue;

      char        *label = extract_label (child_dn);
      SbvLdapNode *node  = sbv_ldap_node_new (child_dn, label);
      g_free (label);

      sbv_ldap_node_set_attrs (node, collect_attrs (ld, entry));

      g_list_store_append (store, node);
      g_object_unref (node);
      ldap_memfree (child_dn);
    }

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_list_store_sort (store, compare_nodes_by_label, NULL);

  g_task_return_pointer (task, store, g_object_unref);
}

/* Public entry — see header. */
void
sbv_ldap_browse_children_async (SbvConnection       *conn,
                                 const char          *parent_dn,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (parent_dn), g_free);
  g_task_run_in_thread (task, children_thread);
  g_object_unref (task);
}

/* Returns the GListStore<SbvLdapNode> of children, or NULL on error. */
GListStore *
sbv_ldap_browse_children_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Attrs (base) ──────────────────────────────────────────────────────── */

/* Worker: BASE search returning the single entry's attribute dump. */
static void
attrs_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  const char    *dn   = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 10, 0 };

  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE,
                               "(objectClass=*)", NULL, 0,
                               NULL, NULL, &tv, 1, &result);
  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Read failed: %s", ldap_err2string (rc));
    return;
  }

  GHashTable  *attrs = NULL;
  LDAPMessage *entry = ldap_first_entry (ld, result);
  if (entry)
    attrs = collect_attrs (ld, entry);
  else
    attrs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify) g_strfreev);

  ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_task_return_pointer (task, attrs, (GDestroyNotify) g_hash_table_unref);
}

/* Public entry — see header. */
void
sbv_ldap_browse_attrs_async (SbvConnection       *conn,
                              const char          *dn,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (dn), g_free);
  g_task_run_in_thread (task, attrs_thread);
  g_object_unref (task);
}

/* Returns the attribute dump (caller owns), or NULL on error. */
GHashTable *
sbv_ldap_browse_attrs_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}
