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

/* Reads the `hasSubordinates` operational attribute from the collected
 * attrs table and translates "TRUE"/"FALSE" → 1/0 (or -1 if absent or
 * unparseable). Lets the UI suppress the tree expander on known-leaf
 * entries. */
static int
parse_has_subordinates (GHashTable *attrs)
{
  if (!attrs) return -1;
  char **vals = g_hash_table_lookup (attrs, "hasSubordinates");
  if (!vals || !vals[0]) return -1;
  if (g_ascii_strcasecmp (vals[0], "TRUE")  == 0) return 1;
  if (g_ascii_strcasecmp (vals[0], "FALSE") == 0) return 0;
  return -1;
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

  /* "*" returns all user attrs; operational attrs like hasSubordinates
   * must be named explicitly, so add it alongside. */
  char *attrs[] = { (char *) "*", (char *) "hasSubordinates", NULL };
  int rc = ldap_search_ext_s (ld, parent_dn, LDAP_SCOPE_ONELEVEL,
                               "(objectClass=*)", attrs, 0,
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

      GHashTable *raw = collect_attrs (ld, entry);
      sbv_ldap_node_set_has_children (node, parse_has_subordinates (raw));
      sbv_ldap_node_set_attrs (node, raw);

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

  char *req_attrs[] = { (char *) "*", (char *) "hasSubordinates", NULL };
  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE,
                               "(objectClass=*)", req_attrs, 0,
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

/* ── Subtree search ────────────────────────────────────────────────────── */

#define SBV_BROWSE_SEARCH_LIMIT 500

typedef struct {
  char *base_dn;
  char *filter;
} SearchData;

static void
search_data_free (SearchData *d)
{
  g_free (d->base_dn);
  g_free (d->filter);
  g_free (d);
}

/* Worker: SUBTREE search with a caller-supplied filter. Truncated results
 * (LDAP_SIZELIMIT_EXCEEDED) are still returned — the panel surfaces the
 * truncation as a banner so the admin knows to refine the filter. */
static void
search_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  SearchData    *d    = task_data;
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

  char *attrs[] = { (char *) "*", (char *) "hasSubordinates", NULL };
  int rc = ldap_search_ext_s (ld, d->base_dn, LDAP_SCOPE_SUBTREE,
                               d->filter, attrs, 0,
                               NULL, NULL, &tv,
                               SBV_BROWSE_SEARCH_LIMIT, &result);
  /* Tolerate size-limit exceeded: results up to the limit are still in `result`. */
  if (rc != LDAP_SUCCESS && rc != LDAP_SIZELIMIT_EXCEEDED) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Search failed: %s", ldap_err2string (rc));
    return;
  }

  GListStore *store = g_list_store_new (SBV_TYPE_LDAP_NODE);

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      char *child_dn = ldap_get_dn (ld, entry);
      if (!child_dn) continue;

      /* For the flat search result, label = full DN so the user can
       * disambiguate same-named entries from different containers. */
      SbvLdapNode *node = sbv_ldap_node_new (child_dn, child_dn);

      GHashTable *raw = collect_attrs (ld, entry);
      sbv_ldap_node_set_has_children (node, parse_has_subordinates (raw));
      sbv_ldap_node_set_attrs (node, raw);

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
sbv_ldap_browse_search_async (SbvConnection       *conn,
                               const char          *base_dn,
                               const char          *filter,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GTask      *task = g_task_new (conn, cancellable, callback, user_data);
  SearchData *d    = g_new0 (SearchData, 1);
  d->base_dn = g_strdup (base_dn);
  d->filter  = g_strdup (filter && *filter ? filter : "(objectClass=*)");
  g_task_set_task_data (task, d, (GDestroyNotify) search_data_free);
  g_task_run_in_thread (task, search_thread);
  g_object_unref (task);
}

/* Returns the GListStore<SbvLdapNode> of matched entries, or NULL on error. */
GListStore *
sbv_ldap_browse_search_finish (SbvConnection *conn,
                                GAsyncResult  *result,
                                GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Per-attribute modify ──────────────────────────────────────────────── */

/* Allocate a single modification descriptor. Copies `attr` and the value
 * vector so the caller can free its inputs once the call returns. */
SbvLdapMod *
sbv_ldap_mod_new (SbvLdapModOp        op,
                   const char         *attr,
                   const char * const *values)
{
  SbvLdapMod *m = g_new0 (SbvLdapMod, 1);
  m->op   = op;
  m->attr = g_strdup (attr);
  if (values && values[0]) {
    guint n = 0;
    while (values[n]) n++;
    m->values = g_new0 (char *, n + 1);
    for (guint i = 0; i < n; i++)
      m->values[i] = g_strdup (values[i]);
  }
  return m;
}

/* Frees a mod descriptor and its owned strings. */
void
sbv_ldap_mod_free (SbvLdapMod *m)
{
  if (!m) return;
  g_free (m->attr);
  g_strfreev (m->values);
  g_free (m);
}

typedef struct {
  char      *dn;
  GPtrArray *mods;   /* GPtrArray<SbvLdapMod*>, owned copies            */
} ModifyData;

static void
modify_data_free (ModifyData *d)
{
  g_free (d->dn);
  if (d->mods) g_ptr_array_unref (d->mods);
  g_free (d);
}

/* Translate the SBV op enum to libldap's LDAP_MOD_* constants. */
static int
op_to_ldap (SbvLdapModOp op)
{
  switch (op) {
    case SBV_LDAP_MOD_ADD:     return LDAP_MOD_ADD;
    case SBV_LDAP_MOD_REPLACE: return LDAP_MOD_REPLACE;
    case SBV_LDAP_MOD_DELETE:  return LDAP_MOD_DELETE;
  }
  return LDAP_MOD_REPLACE;
}

/* Worker: build an LDAPMod array from the staged mods and dispatch a single
 * ldap_modify_ext_s. Order is preserved: the directory applies the mods
 * sequentially, so the panel can stage delete+add for an attribute that
 * needs to change a single value. */
static void
modify_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  ModifyData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  guint n = d->mods ? d->mods->len : 0;
  if (n == 0) {
    sbv_connection_release_ldap (conn);
    g_task_return_boolean (task, TRUE);
    return;
  }

  LDAPMod **lmods = g_new0 (LDAPMod *, n + 1);
  for (guint i = 0; i < n; i++) {
    SbvLdapMod *m  = g_ptr_array_index (d->mods, i);
    LDAPMod    *lm = g_new0 (LDAPMod, 1);
    lm->mod_op       = op_to_ldap (m->op);
    lm->mod_type     = m->attr;
    lm->mod_values   = m->values;   /* aliased — freed below as part of array */
    lmods[i] = lm;
  }

  int rc = ldap_modify_ext_s (ld, d->dn, lmods, NULL, NULL);

  /* Free LDAPMod wrappers without touching the aliased value pointers. */
  for (guint i = 0; i < n; i++) g_free (lmods[i]);
  g_free (lmods);

  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Modify failed: %s", ldap_err2string (rc));
    return;
  }

  g_task_return_boolean (task, TRUE);
}

/* Make a private deep copy of the mods so the caller can free its array
 * regardless of when the worker actually runs. */
static GPtrArray *
copy_mods (GPtrArray *src)
{
  GPtrArray *dst = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_ldap_mod_free);
  if (!src) return dst;
  for (guint i = 0; i < src->len; i++) {
    SbvLdapMod *m = g_ptr_array_index (src, i);
    g_ptr_array_add (dst, sbv_ldap_mod_new (m->op, m->attr,
                                              (const char * const *) m->values));
  }
  return dst;
}

/* Public entry — see header. */
void
sbv_ldap_browse_modify_async (SbvConnection       *conn,
                               const char          *dn,
                               GPtrArray           *mods,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GTask      *task = g_task_new (conn, cancellable, callback, user_data);
  ModifyData *d    = g_new0 (ModifyData, 1);
  d->dn   = g_strdup (dn);
  d->mods = copy_mods (mods);
  g_task_set_task_data (task, d, (GDestroyNotify) modify_data_free);
  g_task_run_in_thread (task, modify_thread);
  g_object_unref (task);
}

/* Returns TRUE on success, FALSE + GError on failure. */
gboolean
sbv_ldap_browse_modify_finish (SbvConnection *conn,
                                GAsyncResult  *result,
                                GError       **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
