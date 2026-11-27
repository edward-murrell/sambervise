#include <glib.h>
#include <gio/gio.h>
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

/* Names sort ignoring case, with a stable tie-break. */
static void
test_name_compare (void)
{
  g_assert_cmpint (sbv_name_compare ("alice", "Bob"), <, 0);
  g_assert_cmpint (sbv_name_compare ("Bob", "alice"), >, 0);
  g_assert_cmpint (sbv_name_compare ("bob", "Bob"), !=, 0);
  g_assert_cmpint (sbv_name_compare (NULL, "a"), <, 0);
  g_assert_cmpint (sbv_name_compare ("x", "x"), ==, 0);
}

/* Users sort by display name, falling back to cn when it is empty. */
static void
test_user_order (void)
{
  const char *display[] = { "zoe Young", NULL, "Adam Smith", "" };
  const char *cn[]      = { "zyoung", "Mary Jones", "asmith", "Brian Lee" };
  const char *want[]    = { "asmith", "Brian Lee", "Mary Jones", "zyoung" };
  GListStore *store = g_list_store_new (SBV_TYPE_USER);
  for (guint i = 0; i < G_N_ELEMENTS (cn); i++) {
    SbvUser *u = sbv_user_new ();
    sbv_user_set_cn (u, cn[i]);
    sbv_user_set_sam (u, cn[i]);
    if (display[i]) sbv_user_set_display_name (u, display[i]);
    g_list_store_append (store, u);
    g_object_unref (u);
  }
  g_list_store_sort (store, sbv_user_compare_by_name, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (want); i++) {
    SbvUser *u = g_list_model_get_item (G_LIST_MODEL (store), i);
    g_assert_cmpstr (sbv_user_get_cn (u), ==, want[i]);
    g_object_unref (u);
  }
  g_object_unref (store);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/search/fold", test_fold);
  g_test_add_func ("/search/matches", test_matches);
  g_test_add_func ("/search/name-compare", test_name_compare);
  g_test_add_func ("/user/order", test_user_order);
  g_test_add_func ("/user/password-expired", test_password_expired);
  return g_test_run ();
}
