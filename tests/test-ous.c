#include <glib.h>
#include <gio/gio.h>
#include "model/sbv-ou.h"

/* Builds an OU with the given kind and path, for the sort tests. */
static SbvOu *
make_ou (SbvOuKind kind, const char *const *path)
{
  SbvOu *ou = sbv_ou_new ();
  sbv_ou_set_kind (ou, kind);
  sbv_ou_set_path (ou, path);
  sbv_ou_set_name (ou, path && path[0] ? path[g_strv_length ((char **) path) - 1] : "root");
  return ou;
}

/* Fresh entries are OUs with no strings set and an empty path. */
static void
test_ou_defaults (void)
{
  SbvOu *ou = sbv_ou_new ();
  g_assert_null (sbv_ou_get_dn (ou));
  g_assert_null (sbv_ou_get_name (ou));
  g_assert_null (sbv_ou_get_description (ou));
  g_assert_null (sbv_ou_get_ldap_attrs (ou));
  g_assert_cmpint (sbv_ou_get_kind (ou), ==, SBV_OU_KIND_OU);
  g_assert_false (sbv_ou_get_critical (ou));
  g_assert_nonnull (sbv_ou_get_path (ou));
  g_assert_cmpuint (sbv_ou_get_depth (ou), ==, 0);
  g_object_unref (ou);
}

/* Setters store copies; set_path(NULL) resets to empty. */
static void
test_ou_setters (void)
{
  SbvOu *ou = sbv_ou_new ();
  const char *path[] = { "Sales", "Sydney", NULL };

  sbv_ou_set_dn (ou, "OU=Sydney,OU=Sales,DC=example,DC=com");
  sbv_ou_set_name (ou, "Sydney");
  sbv_ou_set_description (ou, "Sydney office");
  sbv_ou_set_path (ou, path);

  g_assert_cmpstr (sbv_ou_get_dn (ou), ==, "OU=Sydney,OU=Sales,DC=example,DC=com");
  g_assert_cmpstr (sbv_ou_get_name (ou), ==, "Sydney");
  g_assert_cmpstr (sbv_ou_get_description (ou), ==, "Sydney office");
  g_assert_cmpuint (sbv_ou_get_depth (ou), ==, 2);
  g_assert_cmpstr (sbv_ou_get_path (ou)[1], ==, "Sydney");

  sbv_ou_set_path (ou, NULL);
  g_assert_cmpuint (sbv_ou_get_depth (ou), ==, 0);

  GHashTable *attrs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify) g_strfreev);
  sbv_ou_set_ldap_attrs (ou, attrs);
  g_assert_true (sbv_ou_get_ldap_attrs (ou) == attrs);
  g_object_unref (ou);
}

/* Only non-critical OUs can be renamed, moved or deleted. */
static void
test_ou_editable (void)
{
  SbvOu *ou = sbv_ou_new ();
  g_assert_true (sbv_ou_is_editable (ou));

  sbv_ou_set_critical (ou, TRUE);       /* e.g. OU=Domain Controllers */
  g_assert_false (sbv_ou_is_editable (ou));

  sbv_ou_set_critical (ou, FALSE);
  sbv_ou_set_kind (ou, SBV_OU_KIND_CONTAINER);
  g_assert_false (sbv_ou_is_editable (ou));

  sbv_ou_set_kind (ou, SBV_OU_KIND_DOMAIN);
  g_assert_false (sbv_ou_is_editable (ou));
  g_object_unref (ou);
}

/* Tree order: root first, each parent before its children, siblings
 * sorted case-insensitively. */
static void
test_ou_tree_sort (void)
{
  const char *root[]   = { NULL };
  const char *sales[]  = { "sales", NULL };
  const char *syd[]    = { "sales", "Sydney", NULL };
  const char *adel[]   = { "sales", "Adelaide", NULL };
  const char *users[]  = { "Users", NULL };
  const char *admin[]  = { "Admin", NULL };

  GListStore *store = g_list_store_new (SBV_TYPE_OU);
  SbvOu *items[] = {
    make_ou (SBV_OU_KIND_CONTAINER, users),
    make_ou (SBV_OU_KIND_OU,        syd),
    make_ou (SBV_OU_KIND_DOMAIN,    root),
    make_ou (SBV_OU_KIND_OU,        adel),
    make_ou (SBV_OU_KIND_OU,        sales),
    make_ou (SBV_OU_KIND_OU,        admin),
  };
  for (gsize i = 0; i < G_N_ELEMENTS (items); i++) {
    g_list_store_append (store, items[i]);
    g_object_unref (items[i]);
  }

  g_list_store_sort (store, sbv_ou_compare_tree, NULL);

  const char *expected[] = { "root", "Admin", "sales", "Adelaide", "Sydney", "Users" };
  g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==,
                    G_N_ELEMENTS (expected));
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++) {
    SbvOu *ou = g_list_model_get_item (G_LIST_MODEL (store), i);
    g_assert_cmpstr (sbv_ou_get_name (ou), ==, expected[i]);
    g_object_unref (ou);
  }
  g_object_unref (store);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ou/defaults",  test_ou_defaults);
  g_test_add_func ("/ou/setters",   test_ou_setters);
  g_test_add_func ("/ou/editable",  test_ou_editable);
  g_test_add_func ("/ou/tree-sort", test_ou_tree_sort);
  return g_test_run ();
}
