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

/* Rejects a missing name, a precedence below 1 and a reset window
 * longer than the lockout, even with lockout off. */
static void
test_pso_check (void)
{
  SbvDomainPolicy base = {
    .min_pwd_length = 7, .pwd_history_length = 24,
    .max_pwd_age = sbv_interval_from_days (42, TRUE),
    .min_pwd_age = sbv_interval_from_days (1, FALSE),
    .lockout_duration = sbv_interval_from_minutes (30, TRUE),
    .lockout_observation_window = sbv_interval_from_minutes (30, FALSE),
  };
  SbvPso *pso = sbv_pso_new (&base);
  char   *msg = sbv_pso_check (pso);
  g_assert_nonnull (msg);   /* no name yet */
  g_free (msg);

  pso->name = g_strdup ("  Service accounts ");
  g_assert_null (sbv_pso_check (pso));

  pso->precedence = 0;
  msg = sbv_pso_check (pso);
  g_assert_nonnull (msg);
  g_free (msg);
  pso->precedence = 5;

  pso->policy.lockout_observation_window = sbv_interval_from_minutes (60, FALSE);
  msg = sbv_pso_check (pso);
  g_assert_nonnull (msg);
  g_free (msg);

  pso->policy.lockout_duration = SBV_INTERVAL_NEVER;
  g_assert_null (sbv_pso_check (pso));

  SbvPso *copy = sbv_pso_copy (pso);
  g_assert_cmpstr (copy->name, ==, pso->name);
  g_assert_true (copy->name != pso->name);
  sbv_pso_free (copy);
  sbv_pso_free (pso);
}

/* Sorts by precedence, then name. */
static void
test_pso_compare (void)
{
  GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_pso_free);
  const struct { const char *name; gint64 prec; } in[] = {
    { "b", 20 }, { "z", 10 }, { "a", 20 },
  };
  for (guint i = 0; i < G_N_ELEMENTS (in); i++) {
    SbvPso *p = sbv_pso_new (NULL);
    p->name = g_strdup (in[i].name);
    p->precedence = in[i].prec;
    g_ptr_array_add (a, p);
  }
  g_ptr_array_sort (a, sbv_pso_compare);
  g_assert_cmpstr (((SbvPso *) a->pdata[0])->name, ==, "z");
  g_assert_cmpstr (((SbvPso *) a->pdata[1])->name, ==, "a");
  g_assert_cmpstr (((SbvPso *) a->pdata[2])->name, ==, "b");
  g_ptr_array_unref (a);
}

/* Compares DN lists in any order and treats NULL as empty. */
static void
test_dn_lists_equal (void)
{
  const char *a[] = { "CN=A,DC=x", "CN=B,DC=x", NULL };
  const char *b[] = { "cn=b,dc=x", "CN=A,DC=x", NULL };
  const char *c[] = { "CN=A,DC=x", NULL };
  const char *e[] = { NULL };
  g_assert_true  (sbv_dn_lists_equal (a, b));
  g_assert_false (sbv_dn_lists_equal (a, c));
  g_assert_false (sbv_dn_lists_equal (c, a));
  g_assert_true  (sbv_dn_lists_equal (NULL, e));
  g_assert_false (sbv_dn_lists_equal (NULL, c));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/domain/intervals",    test_intervals);
  g_test_add_func ("/domain/policy-check", test_policy_check);
  g_test_add_func ("/domain/parse-holder", test_parse_holder);
  g_test_add_func ("/domain/pso-check",    test_pso_check);
  g_test_add_func ("/domain/pso-compare",  test_pso_compare);
  g_test_add_func ("/domain/dn-lists-equal", test_dn_lists_equal);
  return g_test_run ();
}
