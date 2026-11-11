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
  g_assert_null   (sbv_user_get_unix_home_dir (user));
  g_assert_null   (sbv_user_get_gecos       (user));

  sbv_user_set_uid_number  (user, 10001);
  sbv_user_set_gid_number  (user, 10001);
  sbv_user_set_login_shell (user, "/bin/bash");
  sbv_user_set_unix_home_dir (user, "/home/jsmith");
  sbv_user_set_gecos       (user, "John Smith");

  g_assert_cmpint (sbv_user_get_uid_number  (user), ==, 10001);
  g_assert_cmpint (sbv_user_get_gid_number  (user), ==, 10001);
  g_assert_cmpstr (sbv_user_get_login_shell (user), ==, "/bin/bash");
  g_assert_cmpstr (sbv_user_get_unix_home_dir (user), ==, "/home/jsmith");
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

/* Windows profile fields default to NULL and store copies. */
static void
test_user_profile (void)
{
  SbvUser *user = sbv_user_new ();

  g_assert_null (sbv_user_get_home_directory (user));
  g_assert_null (sbv_user_get_home_drive     (user));
  g_assert_null (sbv_user_get_script_path    (user));
  g_assert_null (sbv_user_get_profile_path   (user));

  sbv_user_set_home_directory (user, "\\\\fs\\home\\jsmith");
  sbv_user_set_home_drive     (user, "H:");
  sbv_user_set_script_path    (user, "logon.bat");
  sbv_user_set_profile_path   (user, "\\\\fs\\profiles\\jsmith");

  g_assert_cmpstr (sbv_user_get_home_directory (user), ==, "\\\\fs\\home\\jsmith");
  g_assert_cmpstr (sbv_user_get_home_drive     (user), ==, "H:");
  g_assert_cmpstr (sbv_user_get_script_path    (user), ==, "logon.bat");
  g_assert_cmpstr (sbv_user_get_profile_path   (user), ==, "\\\\fs\\profiles\\jsmith");

  /* The Unix home is a separate attribute. */
  g_assert_null (sbv_user_get_unix_home_dir (user));

  sbv_user_set_home_drive (user, NULL);
  g_assert_null (sbv_user_get_home_drive (user));
  g_object_unref (user);
}

/* Drive letters are upper-cased with a colon; anything else is rejected. */
static void
test_user_home_drive (void)
{
  const struct { const char *in; const char *out; } cases[] = {
    { "H:",   "H:" }, { "h:",  "H:" }, { "h",    "H:" }, { " z: ", "Z:" },
    { "",     NULL }, { "HH",  NULL }, { "1:",   NULL }, { "H:\\", NULL },
    { ":",    NULL },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
    char *got = sbv_user_normalise_home_drive (cases[i].in);
    g_assert_cmpstr (got, ==, cases[i].out);
    g_free (got);
  }
  g_assert_null (sbv_user_normalise_home_drive (NULL));
}

/* Lockout follows Samba's rule: timed lockouts end, zero or positive
 * durations last until unlocked, and lockoutTime 0 means not locked. */
static void
test_user_lockout_end (void)
{
  const gint64 min30 = -18000000000LL;      /* 30 minutes, as AD stores it */
  const gint64 t     = 134000000000000000LL; /* some FILETIME */

  g_assert_cmpint (sbv_user_lockout_end (0, min30, t), ==, 0);
  g_assert_cmpint (sbv_user_lockout_end (t, min30, t + 1), ==, t + 18000000000LL);
  g_assert_cmpint (sbv_user_lockout_end (t, min30, t + 18000000000LL), ==, 0);
  g_assert_cmpint (sbv_user_lockout_end (t, 0, t + 1), ==, G_MAXINT64);
  g_assert_cmpint (sbv_user_lockout_end (t, G_MININT64, t + 1), ==, G_MAXINT64);
  g_assert_cmpint (sbv_user_lockout_end (G_MAXINT64 - 5, min30, 0), ==, G_MAXINT64 - 1);

  SbvUser *user = sbv_user_new ();
  g_assert_false (sbv_user_get_locked_out (user));
  g_assert_cmpint (sbv_user_get_lockout_time (user), ==, 0);
  g_assert_cmpint (sbv_user_get_lockout_end (user), ==, 0);
  g_object_unref (user);
}

/* memberOf is stored as given and the primary group defaults to unset. */
static void
test_member_of (void)
{
  SbvUser *u = sbv_user_new ();
  g_assert_null (sbv_user_get_member_of (u));
  g_assert_cmpint (sbv_user_get_primary_group_id (u), ==, -1);

  char **dns = g_new0 (char *, 3);
  dns[0] = g_strdup ("CN=Staff,OU=Groups,DC=example,DC=com");
  dns[1] = g_strdup ("CN=VPN,OU=Groups,DC=example,DC=com");
  sbv_user_set_member_of (u, dns);
  sbv_user_set_primary_group_id (u, 513);

  const char * const *got = sbv_user_get_member_of (u);
  g_assert_cmpstr (got[1], ==, "CN=VPN,OU=Groups,DC=example,DC=com");
  g_assert_null (got[2]);
  g_assert_cmpint (sbv_user_get_primary_group_id (u), ==, 513);
  g_object_unref (u);
}

/* Template paths swap the template's username and expand %username%. */
static void
test_template_path (void)
{
  struct { const char *in, *out; } cases[] = {
    { "\\\\fs1\\profiles\\jtest",     "\\\\fs1\\profiles\\alice" },
    { "\\\\fs1\\home\\JTest$",        "\\\\fs1\\home\\alice$" },
    { "\\\\fs1\\home\\%USERNAME%",    "\\\\fs1\\home\\alice" },
    { "\\\\fs1\\home\\%username%.v6", "\\\\fs1\\home\\alice.v6" },
    { "\\\\fs1\\home\\jtest2",        "\\\\fs1\\home\\jtest2" },
    { "logon.bat",                    "logon.bat" },
    { "scripts/jtest/run.bat",        "scripts/alice/run.bat" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
    char *got = sbv_user_template_path (cases[i].in, "jtest", "alice");
    g_assert_cmpstr (got, ==, cases[i].out);
    g_free (got);
  }
  g_assert_null (sbv_user_template_path ("", "jtest", "alice"));
  g_assert_null (sbv_user_template_path (NULL, "jtest", "alice"));
}

/* Only option bits come from the template. Disabled state stays. */
static void
test_template_uac (void)
{
  /* New user: NORMAL_ACCOUNT | ACCOUNTDISABLE. Template: enabled, password
   * never expires, not delegated. */
  gint64 got = sbv_user_template_uac (0x0202, 0x0200 | 0x10000 | 0x100000);
  g_assert_cmpint (got, ==, 0x0202 | 0x10000 | 0x100000);
  /* Option bits the template lacks are cleared. */
  g_assert_cmpint (sbv_user_template_uac (0x0202 | 0x10000, 0x0200), ==, 0x0202);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/user/properties", test_user_properties);
  g_test_add_func ("/user/rfc2307",    test_user_rfc2307);
  g_test_add_func ("/user/refcount",   test_user_refcount);
  g_test_add_func ("/user/profile",    test_user_profile);
  g_test_add_func ("/user/home-drive", test_user_home_drive);
  g_test_add_func ("/user/lockout-end", test_user_lockout_end);
  g_test_add_func ("/user/member-of", test_member_of);
  g_test_add_func ("/user/template-path", test_template_path);
  g_test_add_func ("/user/template-uac", test_template_uac);
  return g_test_run ();
}
