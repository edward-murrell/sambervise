#include "sbv-ou-backend.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── Shared helpers ─────────────────────────────────────────────────────── */

/* Builds a GError for a failed LDAP call. Common AD refusals get a message
 * an admin can act on; anything else falls back to libldap's text. */
static GError *
ldap_error (const char *what, int rc)
{
  const char *hint = NULL;
  switch (rc) {
    case LDAP_NOT_ALLOWED_ON_NONLEAF:
      hint = "The OU is not empty. Move or delete its contents first.";
      break;
    case LDAP_ALREADY_EXISTS:
      hint = "An object with that name already exists there.";
      break;
    case LDAP_INSUFFICIENT_ACCESS:
      hint = "Access denied. You may lack permission, or the object is "
             "protected from accidental deletion.";
      break;
    case LDAP_NO_SUCH_OBJECT:
      hint = "The object no longer exists. Reload and try again.";
      break;
    case LDAP_UNWILLING_TO_PERFORM:
      hint = "The DC refused. Built-in system objects can't be changed this way.";
      break;
    default:
      break;
  }
  return hint
    ? g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "%s failed: %s", what, hint)
    : g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "%s failed: %s", what,
                   ldap_err2string (rc));
}

/* Returns the first value of `attr` on `entry` as a new string, or NULL. */
static char *
first_value (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  struct berval **bv = ldap_get_values_len (ld, entry, attr);
  char *out = (bv && bv[0]) ? g_strndup (bv[0]->bv_val, bv[0]->bv_len) : NULL;
  ldap_value_free_len (bv);
  return out;
}

/* TRUE if `entry` lists `cls` among its objectClass values. */
static gboolean
has_object_class (LDAP *ld, LDAPMessage *entry, const char *cls)
{
  struct berval **bv = ldap_get_values_len (ld, entry, "objectClass");
  gboolean found = FALSE;
  for (int i = 0; bv && bv[i]; i++)
    if (g_ascii_strncasecmp (bv[i]->bv_val, cls, bv[i]->bv_len) == 0 &&
        strlen (cls) == bv[i]->bv_len)
      found = TRUE;
  ldap_value_free_len (bv);
  return found;
}

/* Collects every attribute on `entry` into a char* → GStrv table for the
 * detail pane. Binary values are shown as a byte count. */
static GHashTable *
collect_raw_attrs (LDAP *ld, LDAPMessage *entry)
{
  GHashTable *raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, (GDestroyNotify) g_strfreev);
  BerElement *ber = NULL;
  for (char *aname = ldap_first_attribute (ld, entry, &ber);
       aname != NULL;
       aname = ldap_next_attribute (ld, entry, ber))
    {
      struct berval **av = ldap_get_values_len (ld, entry, aname);
      if (av) {
        int cnt = ldap_count_values_len (av);
        char **sv = g_new0 (char *, cnt + 1);
        for (int j = 0; j < cnt; j++)
          sv[j] = g_utf8_validate (av[j]->bv_val, (gssize) av[j]->bv_len, NULL)
            ? g_strndup (av[j]->bv_val, av[j]->bv_len)
            : g_strdup_printf ("<binary %lu bytes>", (unsigned long) av[j]->bv_len);
        g_hash_table_insert (raw, g_strdup (aname), sv);
        ldap_value_free_len (av);
      }
      ldap_memfree (aname);
    }
  if (ber) ber_free (ber, 0);
  return raw;
}

/* Builds an SbvOu from one search entry. Returns NULL for entries without
 * a DN or outside `base`. */
static SbvOu *
ou_from_entry (LDAP *ld, LDAPMessage *entry, const char *base)
{
  char *dn = ldap_get_dn (ld, entry);
  if (!dn) return NULL;

  char **path = sbv_dn_path_components (dn, base);
  if (!path) {
    ldap_memfree (dn);
    return NULL;
  }

  SbvOu *ou = sbv_ou_new ();
  sbv_ou_set_dn   (ou, dn);
  sbv_ou_set_path (ou, (const char *const *) path);

  if (path[0] == NULL) {
    char *domain = sbv_dn_domain_name (dn);
    sbv_ou_set_kind (ou, SBV_OU_KIND_DOMAIN);
    sbv_ou_set_name (ou, domain);
    g_free (domain);
  } else {
    char *name = sbv_dn_rdn_value (dn);
    sbv_ou_set_kind (ou, has_object_class (ld, entry, "organizationalUnit")
                           ? SBV_OU_KIND_OU : SBV_OU_KIND_CONTAINER);
    sbv_ou_set_name (ou, name);
    g_free (name);
  }

  char *desc = first_value (ld, entry, "description");
  sbv_ou_set_description (ou, desc);
  g_free (desc);

  char *crit = first_value (ld, entry, "isCriticalSystemObject");
  sbv_ou_set_critical (ou, crit && g_ascii_strcasecmp (crit, "TRUE") == 0);
  g_free (crit);

  sbv_ou_set_ldap_attrs (ou, collect_raw_attrs (ld, entry));

  g_strfreev (path);
  ldap_memfree (dn);
  return ou;
}

/* ── List ───────────────────────────────────────────────────────────────── */

/* Returns a new store holding only entries reachable from the root through
 * visible parents. A container under a hidden one (such as CN=PSPs under
 * CN=System) is left out, as ADUC does. Expects `sorted` in tree order so
 * each parent comes before its children. */
static GListStore *
drop_orphans (GListStore *sorted)
{
  GListStore *out  = g_list_store_new (SBV_TYPE_OU);
  GHashTable *kept = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (sorted));

  for (guint i = 0; i < n; i++) {
    SbvOu *ou = g_list_model_get_item (G_LIST_MODEL (sorted), i);
    const char *dn = sbv_ou_get_dn (ou);
    gboolean keep = (sbv_ou_get_kind (ou) == SBV_OU_KIND_DOMAIN);

    if (!keep && dn) {
      char *parent = NULL;
      if (sbv_dn_split (dn, NULL, &parent)) {
        char *key = g_ascii_strdown (parent, -1);
        keep = g_hash_table_contains (kept, key);
        g_free (key);
      }
      g_free (parent);
    }
    if (keep && dn) {
      g_hash_table_add (kept, g_ascii_strdown (dn, -1));
      g_list_store_append (out, ou);
    }
    g_object_unref (ou);
  }
  g_hash_table_unref (kept);
  return out;
}

/* Worker: reads the domain root with a base search, then every visible OU
 * and container below it with a subtree search. Runs on a GTask thread. */
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

  const char    *base    = sbv_connection_get_base_dn (conn);
  char          *attrs[] = { (char *) "*", NULL };
  struct timeval tv      = { 30, 0 };
  GListStore    *store   = g_list_store_new (SBV_TYPE_OU);

  LDAPMessage *result = NULL;
  int rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_BASE, "(objectClass=*)",
                               attrs, 0, NULL, NULL, &tv, 1, &result);
  if (rc == LDAP_SUCCESS) {
    LDAPMessage *entry = ldap_first_entry (ld, result);
    SbvOu *root = entry ? ou_from_entry (ld, entry, base) : NULL;
    if (root) {
      g_list_store_append (store, root);
      g_object_unref (root);
    }
  }
  if (result) ldap_msgfree (result);
  result = NULL;

  if (rc != LDAP_SUCCESS) {
    sbv_connection_release_ldap (conn);
    g_object_unref (store);
    g_task_return_error (task, ldap_error ("Reading the domain root", rc));
    return;
  }

  const char *filter =
    "(&(|(objectClass=organizationalUnit)(objectClass=container))"
    "(!(showInAdvancedViewOnly=TRUE)))";
  rc = ldap_search_ext_s (ld, base, LDAP_SCOPE_SUBTREE, filter,
                           attrs, 0, NULL, NULL, &tv, LDAP_NO_LIMIT, &result);
  if (rc != LDAP_SUCCESS && rc != LDAP_SIZELIMIT_EXCEEDED) {
    sbv_connection_release_ldap (conn);
    if (result) ldap_msgfree (result);
    g_object_unref (store);
    g_task_return_error (task, ldap_error ("Listing OUs", rc));
    return;
  }

  for (LDAPMessage *entry = ldap_first_entry (ld, result);
       entry != NULL;
       entry = ldap_next_entry (ld, entry))
    {
      SbvOu *ou = ou_from_entry (ld, entry, base);
      if (!ou) continue;
      if (sbv_ou_get_kind (ou) != SBV_OU_KIND_DOMAIN)
        g_list_store_append (store, ou);
      g_object_unref (ou);
    }

  if (result) ldap_msgfree (result);
  sbv_connection_release_ldap (conn);

  g_list_store_sort (store, sbv_ou_compare_tree, NULL);
  GListStore *visible = drop_orphans (store);
  g_object_unref (store);
  g_task_return_pointer (task, visible, g_object_unref);
}

void
sbv_ou_list_async (SbvConnection       *conn,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

GListStore *
sbv_ou_list_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Create ─────────────────────────────────────────────────────────────── */

typedef struct {
  char *parent_dn;
  char *name;
  char *description;
} CreateData;

/* Frees a CreateData payload attached to a GTask. */
static void
create_data_free (CreateData *d)
{
  g_free (d->parent_dn);
  g_free (d->name);
  g_free (d->description);
  g_free (d);
}

/* Worker: adds one organizationalUnit entry. Runs on a GTask thread. */
static void
create_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  CreateData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *escaped = sbv_dn_escape_value (d->name);
  char *dn      = g_strdup_printf ("OU=%s,%s", escaped, d->parent_dn);
  g_free (escaped);

  char *oc_vals[]   = { "top", "organizationalUnit", NULL };
  char *ou_vals[]   = { d->name, NULL };
  char *desc_vals[] = { d->description, NULL };

  LDAPMod oc_mod   = { LDAP_MOD_ADD, "objectClass", { .modv_strvals = oc_vals   } };
  LDAPMod ou_mod   = { LDAP_MOD_ADD, "ou",          { .modv_strvals = ou_vals   } };
  LDAPMod desc_mod = { LDAP_MOD_ADD, "description", { .modv_strvals = desc_vals } };

  LDAPMod *mods[4];
  int n = 0;
  mods[n++] = &oc_mod;
  mods[n++] = &ou_mod;
  if (d->description && *d->description) mods[n++] = &desc_mod;
  mods[n] = NULL;

  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_free (dn);
    g_task_return_error (task, ldap_error ("Create OU", rc));
    return;
  }
  g_task_return_pointer (task, dn, g_free);
}

void
sbv_ou_create_async (SbvConnection       *conn,
                     const char          *parent_dn,
                     const char          *name,
                     const char          *description,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  if (!name || !*name || !parent_dn || !*parent_dn) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Name and parent are required");
    g_object_unref (task);
    return;
  }

  CreateData *d = g_new0 (CreateData, 1);
  d->parent_dn   = g_strdup (parent_dn);
  d->name        = g_strdup (name);
  d->description = g_strdup (description);

  g_task_set_task_data (task, d, (GDestroyNotify) create_data_free);
  g_task_run_in_thread (task, create_thread);
  g_object_unref (task);
}

char *
sbv_ou_create_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Update (rename + description) ──────────────────────────────────────── */

typedef struct {
  char *dn;
  char *new_name;     /* NULL = keep */
  char *description;  /* NULL = leave, "" = clear */
} UpdateData;

/* Frees an UpdateData payload attached to a GTask. */
static void
update_data_free (UpdateData *d)
{
  g_free (d->dn);
  g_free (d->new_name);
  g_free (d->description);
  g_free (d);
}

/* Worker: renames the OU first (RDN change), then replaces the description
 * on the resulting DN. Runs on a GTask thread. */
static void
update_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  UpdateData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  char *effective_dn = g_strdup (d->dn);

  if (d->new_name) {
    char *parent = NULL;
    sbv_dn_split (d->dn, NULL, &parent);
    char *escaped = sbv_dn_escape_value (d->new_name);
    char *new_rdn = g_strdup_printf ("OU=%s", escaped);
    g_free (escaped);

    int rc = ldap_rename_s (ld, d->dn, new_rdn, NULL, 1, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      g_free (new_rdn);
      g_free (parent);
      g_free (effective_dn);
      sbv_connection_release_ldap (conn);
      g_task_return_error (task, ldap_error ("Rename", rc));
      return;
    }
    g_free (effective_dn);
    effective_dn = parent ? g_strdup_printf ("%s,%s", new_rdn, parent)
                          : g_strdup (new_rdn);
    g_free (new_rdn);
    g_free (parent);
  }

  if (d->description) {
    char    *vals[] = { d->description, NULL };
    LDAPMod  mod    = { LDAP_MOD_REPLACE, "description",
                        { .modv_strvals = *d->description ? vals : NULL } };
    LDAPMod *mods[] = { &mod, NULL };
    int rc = ldap_modify_ext_s (ld, effective_dn, mods, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      g_free (effective_dn);
      sbv_connection_release_ldap (conn);
      g_task_return_error (task, ldap_error ("Description update", rc));
      return;
    }
  }

  sbv_connection_release_ldap (conn);
  g_task_return_pointer (task, effective_dn, g_free);
}

void
sbv_ou_update_async (SbvConnection       *conn,
                     SbvOu               *ou,
                     const char          *new_name,
                     const char          *description,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  UpdateData *d = g_new0 (UpdateData, 1);
  d->dn          = g_strdup (sbv_ou_get_dn (ou));
  d->description = g_strdup (description);
  if (new_name && *new_name && g_strcmp0 (new_name, sbv_ou_get_name (ou)) != 0)
    d->new_name = g_strdup (new_name);

  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) update_data_free);
  g_task_run_in_thread (task, update_thread);
  g_object_unref (task);
}

char *
sbv_ou_update_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

/* Worker: deletes one entry. The DC rejects non-empty OUs, which
 * ldap_error() turns into a readable message. Runs on a GTask thread. */
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
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  int rc = ldap_delete_ext_s (ld, dn, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS)
    g_task_return_error (task, ldap_error ("Delete", rc));
  else
    g_task_return_boolean (task, TRUE);
}

void
sbv_ou_delete_async (SbvConnection       *conn,
                     SbvOu               *ou,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (sbv_ou_get_dn (ou)), g_free);
  g_task_run_in_thread (task, delete_thread);
  g_object_unref (task);
}

gboolean
sbv_ou_delete_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Move ───────────────────────────────────────────────────────────────── */

typedef struct {
  char *dn;
  char *new_parent_dn;
} MoveData;

/* Frees a MoveData payload attached to a GTask. */
static void
move_data_free (MoveData *d)
{
  g_free (d->dn);
  g_free (d->new_parent_dn);
  g_free (d);
}

/* Worker: ldap_rename_s with the existing RDN and a new superior, which
 * moves the object without renaming it. Runs on a GTask thread. */
static void
move_thread (GTask *task, gpointer source, gpointer task_data,
             GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  MoveData      *d    = task_data;
  (void) cancellable;

  char *rdn = NULL;
  if (!sbv_dn_split (d->dn, &rdn, NULL)) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Move failed: \"%s\" is not a movable DN", d->dn);
    return;
  }

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_free (rdn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  int rc = ldap_rename_s (ld, d->dn, rdn, d->new_parent_dn, 1, NULL, NULL);
  sbv_connection_release_ldap (conn);

  if (rc != LDAP_SUCCESS) {
    g_free (rdn);
    g_task_return_error (task, ldap_error ("Move", rc));
    return;
  }

  char *new_dn = g_strdup_printf ("%s,%s", rdn, d->new_parent_dn);
  g_free (rdn);
  g_task_return_pointer (task, new_dn, g_free);
}

void
sbv_ou_move_object_async (SbvConnection       *conn,
                          const char          *dn,
                          const char          *new_parent_dn,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);

  char *current_parent = NULL;
  sbv_dn_split (dn, NULL, &current_parent);

  const char *problem = NULL;
  if (!dn || !*dn || !new_parent_dn || !*new_parent_dn)
    problem = "Move failed: an object and a destination are required.";
  else if (sbv_dn_equal (current_parent, new_parent_dn))
    problem = "Move failed: the object is already in that container.";
  else if (sbv_dn_is_within (new_parent_dn, dn))
    problem = "Move failed: an OU can't be moved into itself.";
  g_free (current_parent);

  if (problem) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP, "%s", problem);
    g_object_unref (task);
    return;
  }

  MoveData *d = g_new0 (MoveData, 1);
  d->dn            = g_strdup (dn);
  d->new_parent_dn = g_strdup (new_parent_dn);

  g_task_set_task_data (task, d, (GDestroyNotify) move_data_free);
  g_task_run_in_thread (task, move_thread);
  g_object_unref (task);
}

char *
sbv_ou_move_object_finish (SbvConnection *conn,
                           GAsyncResult  *result,
                           GError       **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}
