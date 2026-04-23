#include <glib.h>
#include "model/sbv-profile.h"
#include "backend/sbv-idmap-hints.h"

static void
test_hints_defaults (void)
{
  SbvProfile *p = sbv_profile_new ();

  /* unset profile → defaults applied */
  SbvIdmapHints *h = sbv_idmap_hints_from_profile (p);
  g_assert_nonnull (h);
  g_assert_cmpint (h->uid_min, ==, SBV_IDMAP_DEFAULT_UID_MIN);
  g_assert_cmpint (h->uid_max, ==, SBV_IDMAP_DEFAULT_UID_MAX);
  g_assert_cmpint (h->gid_min, ==, SBV_IDMAP_DEFAULT_GID_MIN);
  g_assert_cmpint (h->gid_max, ==, SBV_IDMAP_DEFAULT_GID_MAX);
  g_assert_cmpint (h->next_uid_hint, ==, -1);
  g_assert_cmpint (h->next_gid_hint, ==, -1);
  g_assert_nonnull (h->source);
  sbv_idmap_hints_free (h);

  g_object_unref (p);
}

static void
test_hints_from_profile_overrides (void)
{
  SbvProfile *p = sbv_profile_new ();
  sbv_profile_set_uid_min (p, 50000);
  sbv_profile_set_uid_max (p, 59999);
  sbv_profile_set_gid_min (p, 60000);
  sbv_profile_set_gid_max (p, 69999);

  SbvIdmapHints *h = sbv_idmap_hints_from_profile (p);
  g_assert_cmpint (h->uid_min, ==, 50000);
  g_assert_cmpint (h->uid_max, ==, 59999);
  g_assert_cmpint (h->gid_min, ==, 60000);
  g_assert_cmpint (h->gid_max, ==, 69999);
  sbv_idmap_hints_free (h);

  g_object_unref (p);
}

static void
test_profile_range_roundtrip (void)
{
  SbvProfile *p = sbv_profile_new ();
  g_assert_cmpint (sbv_profile_get_uid_min (p), ==, -1);

  sbv_profile_set_uid_min (p, 10000);
  sbv_profile_set_uid_max (p, 19999);
  sbv_profile_set_gid_min (p, 20000);
  sbv_profile_set_gid_max (p, 29999);

  SbvProfile *copy = sbv_profile_copy (p);
  g_assert_cmpint (sbv_profile_get_uid_min (copy), ==, 10000);
  g_assert_cmpint (sbv_profile_get_uid_max (copy), ==, 19999);
  g_assert_cmpint (sbv_profile_get_gid_min (copy), ==, 20000);
  g_assert_cmpint (sbv_profile_get_gid_max (copy), ==, 29999);

  g_object_unref (copy);
  g_object_unref (p);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/idmap/defaults",        test_hints_defaults);
  g_test_add_func ("/idmap/profile_override", test_hints_from_profile_overrides);
  g_test_add_func ("/idmap/profile_roundtrip", test_profile_range_roundtrip);
  return g_test_run ();
}
