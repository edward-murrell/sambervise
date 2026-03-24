#include <glib.h>
#include "model/sbv-group.h"

static void
test_group_properties (void)
{
  SbvGroup *group = sbv_group_new ();

  g_assert_nonnull (group);
  g_assert_null (sbv_group_get_dn (group));
  g_assert_cmpuint (sbv_group_get_member_count (group), ==, 0);

  sbv_group_set_sam (group, "Domain Admins");
  sbv_group_set_display_name (group, "Domain Admins");
  sbv_group_set_dn (group, "CN=Domain Admins,CN=Users,DC=example,DC=com");

  g_assert_cmpstr (sbv_group_get_sam (group),          ==, "Domain Admins");
  g_assert_cmpstr (sbv_group_get_display_name (group), ==, "Domain Admins");
  g_assert_nonnull (sbv_group_get_dn (group));

  char **members = g_new0 (char *, 3);
  members[0] = g_strdup ("CN=Administrator,CN=Users,DC=example,DC=com");
  members[1] = g_strdup ("CN=jsmith,CN=Users,DC=example,DC=com");
  members[2] = NULL;
  sbv_group_set_members (group, members);  /* group takes ownership */

  g_assert_cmpuint (sbv_group_get_member_count (group), ==, 2);

  const char * const *m = sbv_group_get_members (group);
  g_assert_cmpstr (m[0], ==, "CN=Administrator,CN=Users,DC=example,DC=com");
  g_assert_cmpstr (m[1], ==, "CN=jsmith,CN=Users,DC=example,DC=com");
  g_assert_null (m[2]);

  g_object_unref (group);
}

static void
test_group_rfc2307 (void)
{
  SbvGroup *group = sbv_group_new ();

  /* defaults: not set */
  g_assert_cmpint (sbv_group_get_gid_number        (group), ==, -1);
  g_assert_cmpuint (sbv_group_get_member_uid_count (group), ==, 0);
  g_assert_null    (sbv_group_get_member_uid        (group));

  sbv_group_set_gid_number (group, 20001);
  g_assert_cmpint (sbv_group_get_gid_number (group), ==, 20001);

  char **uids = g_new0 (char *, 3);
  uids[0] = g_strdup ("jsmith");
  uids[1] = g_strdup ("bjones");
  uids[2] = NULL;
  sbv_group_set_member_uid (group, uids);  /* group takes ownership */

  g_assert_cmpuint (sbv_group_get_member_uid_count (group), ==, 2);
  const char * const *u = sbv_group_get_member_uid (group);
  g_assert_cmpstr (u[0], ==, "jsmith");
  g_assert_cmpstr (u[1], ==, "bjones");
  g_assert_null   (u[2]);

  /* replacing frees old array */
  char **uids2 = g_new0 (char *, 1);
  uids2[0] = NULL;
  sbv_group_set_member_uid (group, uids2);
  g_assert_cmpuint (sbv_group_get_member_uid_count (group), ==, 0);

  g_object_unref (group);
}

static void
test_group_members_replaced (void)
{
  SbvGroup *group = sbv_group_new ();

  char **m1 = g_new0 (char *, 2);
  m1[0] = g_strdup ("CN=user1,DC=example,DC=com");
  sbv_group_set_members (group, m1);
  g_assert_cmpuint (sbv_group_get_member_count (group), ==, 1);

  char **m2 = g_new0 (char *, 1);
  m2[0] = NULL;
  sbv_group_set_members (group, m2);
  g_assert_cmpuint (sbv_group_get_member_count (group), ==, 0);

  g_object_unref (group);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/group/properties",        test_group_properties);
  g_test_add_func ("/group/rfc2307",           test_group_rfc2307);
  g_test_add_func ("/group/members_replaced",  test_group_members_replaced);
  return g_test_run ();
}
