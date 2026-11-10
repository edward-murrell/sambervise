#include <glib.h>
#include "model/sbv-search.h"
#include "model/sbv-user.h"

/* Folding trims, lower-cases and turns blank text into NULL. */
static void
test_fold (void)
{
  char *f = sbv_search_fold ("  JaCky ");
  g_assert_cmpstr (f, ==, "jacky");
  g_free (f);
  g_assert_null (sbv_search_fold ("   "));
  g_assert_null (sbv_search_fold (""));
  g_assert_null (sbv_search_fold (NULL));
}

/* A missing first field must not stop later fields being searched. */
static void
test_matches (void)
{
  const char *fields[] = { NULL, "Jackie Tester", "jtest", NULL };
  g_assert_true  (sbv_search_matches (NULL, fields, G_N_ELEMENTS (fields)));
  g_assert_true  (sbv_search_matches ("jtest", fields, G_N_ELEMENTS (fields)));
  g_assert_true  (sbv_search_matches ("tester", fields, G_N_ELEMENTS (fields)));
  g_assert_false (sbv_search_matches ("nicole", fields, G_N_ELEMENTS (fields)));
}

#define DAY      (864000000000LL)          /* one day in FILETIME ticks */
#define MAX_AGE  (-42 * DAY)               /* 42-day maxPwdAge */
#define NOW      (134400000000000000LL)

/* Covers the AD password expiry rule used when the computed flag is
 * missing. */
static void
test_password_expired (void)
{
  /* Recent password: not expired. */
  g_assert_false (sbv_user_password_expired (0x200, NOW - DAY, MAX_AGE, NOW));
  /* Older than maxPwdAge: expired. */
  g_assert_true  (sbv_user_password_expired (0x200, NOW - 43 * DAY, MAX_AGE, NOW));
  /* Must change at next logon: expired. */
  g_assert_true  (sbv_user_password_expired (0x200, 0, MAX_AGE, NOW));
  /* Password never expires, even with pwdLastSet 0. */
  g_assert_false (sbv_user_password_expired (0x10200, 0, MAX_AGE, NOW));
  /* Workstation trust accounts never expire. */
  g_assert_false (sbv_user_password_expired (0x1000, NOW - 400 * DAY, MAX_AGE, NOW));
  /* No maximum age set. */
  g_assert_false (sbv_user_password_expired (0x200, NOW - 400 * DAY, 0, NOW));
  g_assert_false (sbv_user_password_expired (0x200, NOW - 400 * DAY, G_MININT64, NOW));
  /* pwdLastSet missing. */
  g_assert_false (sbv_user_password_expired (0x200, -1, MAX_AGE, NOW));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/search/fold", test_fold);
  g_test_add_func ("/search/matches", test_matches);
  g_test_add_func ("/user/password-expired", test_password_expired);
  return g_test_run ();
}
