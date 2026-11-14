#include <glib.h>
#include "model/sbv-domain.h"

/* Days and minutes round-trip, and zero and "never" read as 0. */
static void
test_intervals (void)
{
  g_assert_cmpint (sbv_interval_to_days (G_GINT64_CONSTANT (-36288000000000)), ==, 42);
  g_assert_cmpint (sbv_interval_to_minutes (G_GINT64_CONSTANT (-18000000000)), ==, 30);
  g_assert_cmpint (sbv_interval_to_days (SBV_INTERVAL_NEVER), ==, 0);
  g_assert_cmpint (sbv_interval_to_days (0), ==, 0);

  g_assert_cmpint (sbv_interval_from_days (42, TRUE), ==, G_GINT64_CONSTANT (-36288000000000));
  g_assert_cmpint (sbv_interval_from_days (0, TRUE),  ==, SBV_INTERVAL_NEVER);
  g_assert_cmpint (sbv_interval_from_days (0, FALSE), ==, 0);
  g_assert_cmpint (sbv_interval_from_minutes (30, FALSE), ==, G_GINT64_CONSTANT (-18000000000));
  g_assert_cmpint (sbv_interval_from_minutes (0, TRUE), ==, SBV_INTERVAL_NEVER);

  /* Half a day rounds up. */
  g_assert_cmpint (sbv_interval_to_days (G_GINT64_CONSTANT (-432000000000)), ==, 1);
}

/* Rejects a minimum age at or above the maximum, and a reset window
 * longer than the lockout duration. */
static void
test_policy_check (void)
{
  SbvDomainPolicy p = {
    .min_pwd_length = 7, .pwd_history_length = 24,
    .max_pwd_age = sbv_interval_from_days (42, TRUE),
    .min_pwd_age = sbv_interval_from_days (1, FALSE),
    .pwd_properties = SBV_PWD_COMPLEX,
    .lockout_threshold = 0,
    .lockout_duration = sbv_interval_from_minutes (30, TRUE),
    .lockout_observation_window = sbv_interval_from_minutes (30, FALSE),
  };
  char *msg = sbv_domain_policy_check (&p);
  g_assert_null (msg);

  p.min_pwd_age = sbv_interval_from_days (42, FALSE);
  msg = sbv_domain_policy_check (&p);
  g_assert_nonnull (msg);
  g_free (msg);

  /* Never expiring allows any minimum age. */
  p.max_pwd_age = SBV_INTERVAL_NEVER;
  msg = sbv_domain_policy_check (&p);
  g_assert_null (msg);

  p.lockout_threshold = 5;
  p.lockout_observation_window = sbv_interval_from_minutes (60, FALSE);
  msg = sbv_domain_policy_check (&p);
  g_assert_nonnull (msg);
  g_free (msg);

  /* Locked until an admin unlocks allows any window. */
  p.lockout_duration = SBV_INTERVAL_NEVER;
  msg = sbv_domain_policy_check (&p);
  g_assert_null (msg);
}

/* Pulls the server and site out of an NTDS Settings DN. */
static void
test_parse_holder (void)
{
  char *server = NULL, *server_dn = NULL, *site = NULL;
  gboolean deleted = TRUE;

  g_assert_true (sbv_fsmo_parse_holder (
    "CN=NTDS Settings,CN=DC1,CN=Servers,CN=Default-First-Site-Name,"
    "CN=Sites,CN=Configuration,DC=example,DC=com",
    &server, &server_dn, &site, &deleted));
  g_assert_cmpstr (server, ==, "DC1");
  g_assert_cmpstr (server_dn, ==, "CN=DC1,CN=Servers,CN=Default-First-Site-Name,"
                                  "CN=Sites,CN=Configuration,DC=example,DC=com");
  g_assert_cmpstr (site, ==, "Default-First-Site-Name");
  g_assert_false (deleted);
  g_free (server); g_free (server_dn); g_free (site);

  g_assert_true (sbv_fsmo_parse_holder (
    "CN=NTDS Settings\\0ADEL:1234,CN=OLDDC,CN=Servers,CN=S,CN=Sites,"
    "CN=Configuration,DC=example,DC=com",
    &server, NULL, NULL, &deleted));
  g_assert_cmpstr (server, ==, "OLDDC");
  g_assert_true (deleted);
  g_free (server);

  g_assert_false (sbv_fsmo_parse_holder ("DC=com", &server, NULL, NULL, NULL));
  g_assert_null (server);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/domain/intervals",    test_intervals);
  g_test_add_func ("/domain/policy-check", test_policy_check);
  g_test_add_func ("/domain/parse-holder", test_parse_holder);
  return g_test_run ();
}
