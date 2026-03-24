#include <glib.h>
#include "model/sbv-user.h"

static void
test_user_properties (void)
{
  SbvUser *user = sbv_user_new ();

  g_assert_nonnull (user);
  g_assert_true (sbv_user_get_enabled (user));  /* default: enabled */
  g_assert_null (sbv_user_get_dn (user));

  sbv_user_set_sam (user, "jsmith");
  sbv_user_set_display_name (user, "John Smith");
  sbv_user_set_email (user, "jsmith@example.com");
  sbv_user_set_enabled (user, FALSE);

  g_assert_cmpstr (sbv_user_get_sam (user),          ==, "jsmith");
  g_assert_cmpstr (sbv_user_get_display_name (user), ==, "John Smith");
  g_assert_cmpstr (sbv_user_get_email (user),        ==, "jsmith@example.com");
  g_assert_false  (sbv_user_get_enabled (user));

  /* Overwrite a field */
  sbv_user_set_sam (user, "jsmith2");
  g_assert_cmpstr (sbv_user_get_sam (user), ==, "jsmith2");

  g_object_unref (user);
}

static void
test_user_rfc2307 (void)
{
  SbvUser *user = sbv_user_new ();

  /* defaults: not set */
  g_assert_cmpint (sbv_user_get_uid_number  (user), ==, -1);
  g_assert_cmpint (sbv_user_get_gid_number  (user), ==, -1);
  g_assert_null   (sbv_user_get_login_shell (user));
  g_assert_null   (sbv_user_get_home_dir    (user));
  g_assert_null   (sbv_user_get_gecos       (user));

  sbv_user_set_uid_number  (user, 10001);
  sbv_user_set_gid_number  (user, 10001);
  sbv_user_set_login_shell (user, "/bin/bash");
  sbv_user_set_home_dir    (user, "/home/jsmith");
  sbv_user_set_gecos       (user, "John Smith");

  g_assert_cmpint (sbv_user_get_uid_number  (user), ==, 10001);
  g_assert_cmpint (sbv_user_get_gid_number  (user), ==, 10001);
  g_assert_cmpstr (sbv_user_get_login_shell (user), ==, "/bin/bash");
  g_assert_cmpstr (sbv_user_get_home_dir    (user), ==, "/home/jsmith");
  g_assert_cmpstr (sbv_user_get_gecos       (user), ==, "John Smith");

  /* overwrite string field */
  sbv_user_set_login_shell (user, "/bin/sh");
  g_assert_cmpstr (sbv_user_get_login_shell (user), ==, "/bin/sh");

  g_object_unref (user);
}

static void
test_user_refcount (void)
{
  SbvUser *user = sbv_user_new ();
  g_object_ref (user);
  g_object_unref (user);
  /* Should still be alive */
  sbv_user_set_sam (user, "alive");
  g_assert_cmpstr (sbv_user_get_sam (user), ==, "alive");
  g_object_unref (user);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/user/properties", test_user_properties);
  g_test_add_func ("/user/rfc2307",    test_user_rfc2307);
  g_test_add_func ("/user/refcount",   test_user_refcount);
  return g_test_run ();
}
