#include "sbv-sites-backend.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <string.h>

/* LDAP_SERVER_TREE_DELETE_OID: delete an object and everything under it. */
#define TREE_DELETE_OID "1.2.840.113556.1.4.805"

void
sbv_sites_info_free (SbvSitesInfo *info)
{
  if (!info) return;
  g_free (info->sites_dn);
  if (info->sites)   g_ptr_array_unref (info->sites);
  if (info->subnets) g_ptr_array_unref (info->subnets);
  g_free (info);
}

/* ── Helpers (caller holds the LDAP lock) ───────────────────────────────── */

/* Builds an error from `rc` plus the server's diagnostic text, if any. */
static GError *
ldap_error (LDAP *ld, int rc, const char *what)
{
  char *diag = NULL;
  ldap_get_option (ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag);
  GError *err = g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "%s: %s%s%s%s", what,
                             ldap_err2string (rc),
                             diag && *diag ? " (" : "",
                             diag && *diag ? diag : "",
                             diag && *diag ? ")" : "");
  if (diag) ldap_memfree (diag);
  return err;
}

/* Returns `attr` of `entry` as a string, or NULL. Caller frees. */
static char *
entry_str (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  struct berval **bv  = ldap_get_values_len (ld, entry, attr);
  char           *out = (bv && bv[0]) ? g_strndup (bv[0]->bv_val, bv[0]->bv_len) : NULL;
  ldap_value_free_len (bv);
  return out;
}

/* Says whether `entry` has `cls` among its objectClass values. */
static gboolean
has_class (LDAP *ld, LDAPMessage *entry, const char *cls)
{
  struct berval **bv    = ldap_get_values_len (ld, entry, "objectClass");
  gboolean        found = FALSE;
  for (int i = 0; bv && bv[i] && !found; i++)
    found = bv[i]->bv_len == strlen (cls)
            && g_ascii_strncasecmp (bv[i]->bv_val, cls, bv[i]->bv_len) == 0;
  ldap_value_free_len (bv);
  return found;
}

/* Returns TRUE when `s` holds more than white space. */
static gboolean
has_text (const char *s)
{
  if (!s) return FALSE;
  for (; *s; s++)
    if (!g_ascii_isspace (*s)) return TRUE;
  return FALSE;
}

/* Builds a one-value modification on the stack-friendly arrays given. */
static LDAPMod *
fill_mod (LDAPMod *m, char *vals[2], int op, const char *attr, const char *value)
{
  vals[0] = (char *) value;
  vals[1] = NULL;
  m->mod_op     = op;
  m->mod_type   = (char *) attr;
  m->mod_values = value ? vals : NULL;
  return m;
}

/* Orders strings in a GPtrArray, ignoring ASCII case. */
static gint
str_ptr_compare (gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp (*(const char * const *) a, *(const char * const *) b);
}

/* ── List ───────────────────────────────────────────────────────────────── */

/* Worker: reads the Configuration DN, then one subtree search of
 * CN=Sites for sites, servers and subnets. Runs on a GTask thread. */
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
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  struct timeval tv        = { 30, 0 };
  char          *dse_attrs[] = { (char *) "configurationNamingContext", NULL };
  LDAPMessage   *dse       = NULL;
  int rc = ldap_search_ext_s (ld, "", LDAP_SCOPE_BASE, "(objectClass=*)",
                              dse_attrs, 0, NULL, NULL, &tv, 1, &dse);
  LDAPMessage *dse_entry = rc == LDAP_SUCCESS ? ldap_first_entry (ld, dse) : NULL;
  char        *config    = dse_entry ? entry_str (ld, dse_entry, "configurationNamingContext") : NULL;
  if (dse) ldap_msgfree (dse);
  if (!config) {
    GError *err = ldap_error (ld, rc == LDAP_SUCCESS ? LDAP_NO_SUCH_OBJECT : rc,
                              "Couldn't find the Configuration partition");
    sbv_connection_release_ldap (conn);
    g_task_return_error (task, err);
    return;
  }

  SbvSitesInfo *info = g_new0 (SbvSitesInfo, 1);
  info->sites_dn = g_strdup_printf ("CN=Sites,%s", config);
  info->sites    = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_site_free);
  info->subnets  = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_subnet_free);
  g_free (config);

  char *attrs[] = { (char *) "objectClass", (char *) "cn", (char *) "description",
                    (char *) "dNSHostName", (char *) "siteObject", NULL };
  LDAPMessage *res = NULL;
  rc = ldap_search_ext_s (ld, info->sites_dn, LDAP_SCOPE_SUBTREE,
                          "(|(objectClass=site)(objectClass=server)(objectClass=subnet))",
                          attrs, 0, NULL, NULL, &tv, LDAP_NO_LIMIT, &res);
  if (rc != LDAP_SUCCESS) {
    GError *err = ldap_error (ld, rc, "Couldn't list sites");
    if (res) ldap_msgfree (res);
    sbv_connection_release_ldap (conn);
    sbv_sites_info_free (info);
    g_task_return_error (task, err);
    return;
  }

  /* Server names by lower-cased site DN, attached once every site is in. */
  GHashTable *servers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify) g_ptr_array_unref);
  for (LDAPMessage *e = ldap_first_entry (ld, res); e; e = ldap_next_entry (ld, e)) {
    char *dn_raw = ldap_get_dn (ld, e);
    char *dn     = g_strdup (dn_raw);
    ldap_memfree (dn_raw);

    if (has_class (ld, e, "site")) {
      SbvSite *s     = g_new0 (SbvSite, 1);
      s->dn          = g_strdup (dn);
      s->name        = entry_str (ld, e, "cn");
      if (!s->name) s->name = sbv_dn_rdn_value (dn);
      s->description = entry_str (ld, e, "description");
      g_ptr_array_add (info->sites, s);
    } else if (has_class (ld, e, "subnet")) {
      SbvSubnet *s   = g_new0 (SbvSubnet, 1);
      s->dn          = g_strdup (dn);
      s->name        = entry_str (ld, e, "cn");
      if (!s->name) s->name = sbv_dn_rdn_value (dn);
      s->description = entry_str (ld, e, "description");
      s->site_dn     = entry_str (ld, e, "siteObject");
      g_ptr_array_add (info->subnets, s);
    } else if (has_class (ld, e, "server")) {
      char *site = sbv_site_dn_for_server (dn);
      if (site) {
        char      *key  = g_ascii_strdown (site, -1);
        GPtrArray *list = g_hash_table_lookup (servers, key);
        if (!list) {
          list = g_ptr_array_new_with_free_func (g_free);
          g_hash_table_insert (servers, g_strdup (key), list);
        }
        char *host = entry_str (ld, e, "dNSHostName");
        g_ptr_array_add (list, host ? host : sbv_dn_rdn_value (dn));
        g_free (key);
        g_free (site);
      }
    }
    g_free (dn);
  }
  ldap_msgfree (res);
  sbv_connection_release_ldap (conn);

  for (guint i = 0; i < info->sites->len; i++) {
    SbvSite   *s    = g_ptr_array_index (info->sites, i);
    char      *key  = g_ascii_strdown (s->dn, -1);
    GPtrArray *list = g_hash_table_lookup (servers, key);
    GPtrArray *out  = g_ptr_array_new ();
    for (guint j = 0; list && j < list->len; j++)
      g_ptr_array_add (out, g_strdup (g_ptr_array_index (list, j)));
    g_ptr_array_sort (out, str_ptr_compare);
    g_ptr_array_add (out, NULL);
    s->servers = (char **) g_ptr_array_free (out, FALSE);
    g_free (key);
  }
  g_hash_table_unref (servers);

  g_ptr_array_sort (info->sites,   sbv_site_compare);
  g_ptr_array_sort (info->subnets, sbv_subnet_compare);
  g_task_return_pointer (task, info, (GDestroyNotify) sbv_sites_info_free);
}

void
sbv_sites_list_async (SbvConnection       *conn,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, list_thread);
  g_object_unref (task);
}

SbvSitesInfo *
sbv_sites_list_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Changes ────────────────────────────────────────────────────────────── */

typedef enum {
  OP_SITE_CREATE,
  OP_SITE_UPDATE,
  OP_SITE_DELETE,
  OP_SUBNET_CREATE,
  OP_SUBNET_UPDATE,
  OP_SUBNET_DELETE,
} OpKind;

/* One change. Which fields are set depends on `kind`. */
typedef struct {
  OpKind  kind;
  char   *dn;        /* the object, or its parent for a create */
  char   *old_name;
  char   *new_name;
  char   *old_desc;
  char   *new_desc;
  char   *old_site;
  char   *new_site;
} OpData;

/* Frees an OpData. */
static void
op_data_free (OpData *d)
{
  g_free (d->dn);
  g_free (d->old_name);
  g_free (d->new_name);
  g_free (d->old_desc);
  g_free (d->new_desc);
  g_free (d->old_site);
  g_free (d->new_site);
  g_free (d);
}

/* Adds a replace or delete of `attr` to `mods` when it changed. A value
 * of only white space counts as none. */
static void
diff_attr (LDAPMod **mods, int *n, LDAPMod *buf, char *vals[][2],
           const char *attr, const char *old_value, const char *new_value)
{
  gboolean had = has_text (old_value);
  gboolean has = has_text (new_value);
  if (has && (!had || strcmp (old_value, new_value) != 0)) {
    mods[*n] = fill_mod (&buf[*n], vals[*n], LDAP_MOD_REPLACE, attr, new_value);
    (*n)++;
  } else if (!has && had) {
    mods[*n] = fill_mod (&buf[*n], vals[*n], LDAP_MOD_DELETE, attr, NULL);
    (*n)++;
  }
}

/* Applies the description diff, and for subnets the site diff, to `dn`.
 * Returns an LDAP result code. */
static int
modify_changed (LDAP *ld, OpData *d)
{
  LDAPMod  buf[2];
  LDAPMod *mods[3];
  char    *vals[2][2];
  int      n = 0;
  diff_attr (mods, &n, buf, vals, "description", d->old_desc, d->new_desc);
  if (d->kind == OP_SUBNET_UPDATE)
    diff_attr (mods, &n, buf, vals, "siteObject", d->old_site, d->new_site);
  mods[n] = NULL;
  if (n == 0) return LDAP_SUCCESS;
  return ldap_modify_ext_s (ld, d->dn, mods, NULL, NULL);
}

/* Adds a child of `parent` with one object class. Returns an LDAP
 * result code. */
static int
add_child (LDAP *ld, const char *parent, const char *cn, const char *cls)
{
  char    *dn = sbv_dn_build ("CN", cn, parent);
  LDAPMod  m;
  char    *vals[2];
  LDAPMod *mods[] = { fill_mod (&m, vals, LDAP_MOD_ADD, "objectClass", cls), NULL };
  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  g_free (dn);
  return rc;
}

/* Deletes `dn` and everything under it. Returns an LDAP result code. */
static int
tree_delete (LDAP *ld, const char *dn)
{
  LDAPControl  ctrl     = { (char *) TREE_DELETE_OID, { 0, NULL }, 1 };
  LDAPControl *ctrls[]  = { &ctrl, NULL };
  return ldap_delete_ext_s (ld, dn, ctrls, NULL);
}

/* Creates a site and its two standard children. Removes the site again
 * if a child fails. */
static GError *
site_create (LDAP *ld, OpData *d)
{
  char    *dn = sbv_dn_build ("CN", d->new_name, d->dn);
  LDAPMod  buf[2];
  char    *vals[2][2];
  LDAPMod *mods[3];
  int      n = 0;
  mods[n] = fill_mod (&buf[n], vals[n], LDAP_MOD_ADD, "objectClass", "site"); n++;
  if (has_text (d->new_desc)) {
    mods[n] = fill_mod (&buf[n], vals[n], LDAP_MOD_ADD, "description", d->new_desc);
    n++;
  }
  mods[n] = NULL;

  GError *err = NULL;
  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  if (rc != LDAP_SUCCESS) {
    err = ldap_error (ld, rc, "Couldn't create the site");
  } else {
    rc = add_child (ld, dn, "NTDS Site Settings", "nTDSSiteSettings");
    if (rc == LDAP_SUCCESS)
      rc = add_child (ld, dn, "Servers", "serversContainer");
    if (rc != LDAP_SUCCESS) {
      err = ldap_error (ld, rc, "Couldn't finish creating the site");
      tree_delete (ld, dn);
    }
  }
  g_free (dn);
  return err;
}

/* Deletes a site after checking that no DC sits in it. */
static GError *
site_delete (LDAP *ld, OpData *d)
{
  char          *servers = sbv_dn_build ("CN", "Servers", d->dn);
  char          *attrs[] = { (char *) "1.1", NULL };
  struct timeval tv      = { 30, 0 };
  LDAPMessage   *res     = NULL;
  int rc = ldap_search_ext_s (ld, servers, LDAP_SCOPE_ONELEVEL, "(objectClass=server)",
                              attrs, 0, NULL, NULL, &tv, 1, &res);
  g_free (servers);
  gboolean has_dc = (rc == LDAP_SUCCESS || rc == LDAP_SIZELIMIT_EXCEEDED)
                    && res && ldap_first_entry (ld, res);
  if (res) ldap_msgfree (res);
  if (has_dc)
    return g_error_new (SBV_ERROR, SBV_ERROR_LDAP,
                        "A DC still belongs to this site. Move it to another site first.");
  if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_OBJECT)
    return ldap_error (ld, rc, "Couldn't check the site for DCs");

  rc = tree_delete (ld, d->dn);
  return rc == LDAP_SUCCESS ? NULL : ldap_error (ld, rc, "Couldn't delete the site");
}

/* Creates a subnet with its site and description. */
static GError *
subnet_create (LDAP *ld, OpData *d)
{
  char    *parent = sbv_dn_build ("CN", "Subnets", d->dn);
  char    *dn     = sbv_dn_build ("CN", d->new_name, parent);
  LDAPMod  buf[3];
  char    *vals[3][2];
  LDAPMod *mods[4];
  int      n = 0;
  mods[n] = fill_mod (&buf[n], vals[n], LDAP_MOD_ADD, "objectClass", "subnet"); n++;
  if (d->new_site) {
    mods[n] = fill_mod (&buf[n], vals[n], LDAP_MOD_ADD, "siteObject", d->new_site);
    n++;
  }
  if (has_text (d->new_desc)) {
    mods[n] = fill_mod (&buf[n], vals[n], LDAP_MOD_ADD, "description", d->new_desc);
    n++;
  }
  mods[n] = NULL;
  int rc = ldap_add_ext_s (ld, dn, mods, NULL, NULL);
  g_free (parent);
  g_free (dn);
  return rc == LDAP_SUCCESS ? NULL : ldap_error (ld, rc, "Couldn't create the subnet");
}

/* Worker: runs one change. Runs on a GTask thread. */
static void
op_thread (GTask *task, gpointer source, gpointer task_data,
           GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  OpData        *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  GError *err = NULL;
  int     rc;
  switch (d->kind) {
  case OP_SITE_CREATE:
    err = site_create (ld, d);
    break;
  case OP_SITE_UPDATE:
    rc = modify_changed (ld, d);
    if (rc != LDAP_SUCCESS) {
      err = ldap_error (ld, rc, "Couldn't save the site");
    } else if (g_strcmp0 (d->old_name, d->new_name) != 0) {
      char *rdn = sbv_dn_build ("CN", d->new_name, NULL);
      rc = ldap_rename_s (ld, d->dn, rdn, NULL, 1, NULL, NULL);
      if (rc != LDAP_SUCCESS)
        err = ldap_error (ld, rc, "Saved the description, but couldn't rename the site");
      g_free (rdn);
    }
    break;
  case OP_SITE_DELETE:
    err = site_delete (ld, d);
    break;
  case OP_SUBNET_CREATE:
    err = subnet_create (ld, d);
    break;
  case OP_SUBNET_UPDATE:
    rc = modify_changed (ld, d);
    if (rc != LDAP_SUCCESS) err = ldap_error (ld, rc, "Couldn't save the subnet");
    break;
  case OP_SUBNET_DELETE:
    rc = ldap_delete_ext_s (ld, d->dn, NULL, NULL);
    if (rc != LDAP_SUCCESS) err = ldap_error (ld, rc, "Couldn't delete the subnet");
    break;
  }
  sbv_connection_release_ldap (conn);

  if (err) g_task_return_error (task, err);
  else     g_task_return_boolean (task, TRUE);
}

/* Starts `d` on a worker thread. Takes ownership of `d`. */
static void
run_op (SbvConnection *conn, OpData *d, GCancellable *cancellable,
        GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) op_data_free);
  g_task_run_in_thread (task, op_thread);
  g_object_unref (task);
}

/* Makes an OpData of `kind` on `dn`. */
static OpData *
op_new (OpKind kind, const char *dn)
{
  OpData *d = g_new0 (OpData, 1);
  d->kind = kind;
  d->dn   = g_strdup (dn);
  return d;
}

void
sbv_site_create_async (SbvConnection *conn, const char *sites_dn,
                       const char *name, const char *description,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer user_data)
{
  OpData *d   = op_new (OP_SITE_CREATE, sites_dn);
  d->new_name = g_strdup (name);
  d->new_desc = g_strdup (description);
  run_op (conn, d, cancellable, callback, user_data);
}

void
sbv_site_update_async (SbvConnection *conn, const char *dn,
                       const char *old_name, const char *new_name,
                       const char *old_description, const char *new_description,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer user_data)
{
  OpData *d   = op_new (OP_SITE_UPDATE, dn);
  d->old_name = g_strdup (old_name);
  d->new_name = g_strdup (new_name);
  d->old_desc = g_strdup (old_description);
  d->new_desc = g_strdup (new_description);
  run_op (conn, d, cancellable, callback, user_data);
}

void
sbv_site_delete_async (SbvConnection *conn, const char *dn,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer user_data)
{
  run_op (conn, op_new (OP_SITE_DELETE, dn), cancellable, callback, user_data);
}

void
sbv_subnet_create_async (SbvConnection *conn, const char *sites_dn,
                         const SbvSubnet *subnet, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  OpData *d   = op_new (OP_SUBNET_CREATE, sites_dn);
  d->new_name = g_strdup (subnet->name);
  d->new_desc = g_strdup (subnet->description);
  d->new_site = g_strdup (subnet->site_dn);
  run_op (conn, d, cancellable, callback, user_data);
}

void
sbv_subnet_update_async (SbvConnection *conn, const SbvSubnet *original,
                         const SbvSubnet *updated, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  OpData *d   = op_new (OP_SUBNET_UPDATE, original->dn);
  d->old_desc = g_strdup (original->description);
  d->new_desc = g_strdup (updated->description);
  d->old_site = g_strdup (original->site_dn);
  d->new_site = g_strdup (updated->site_dn);
  run_op (conn, d, cancellable, callback, user_data);
}

void
sbv_subnet_delete_async (SbvConnection *conn, const char *dn,
                         GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer user_data)
{
  run_op (conn, op_new (OP_SUBNET_DELETE, dn), cancellable, callback, user_data);
}

gboolean
sbv_sites_op_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
