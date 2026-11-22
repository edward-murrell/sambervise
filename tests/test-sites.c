#include <glib.h>
#include <string.h>
#include "model/sbv-site.h"

/* Checks that `text` normalises to `want`. */
static void
expect_subnet (const char *text, const char *want)
{
  char *err = NULL;
  char *got = sbv_subnet_normalise (text, &err);
  g_assert_null (err);
  g_assert_cmpstr (got, ==, want);
  g_free (got);
}

/* Checks that `text` is rejected with a message. */
static void
expect_bad_subnet (const char *text)
{
  char *err = NULL;
  char *got = sbv_subnet_normalise (text, &err);
  g_assert_null (got);
  g_assert_nonnull (err);
  g_free (err);
}

/* Accepts what Samba accepts and rejects what it refuses. */
static void
test_subnet_normalise (void)
{
  expect_subnet ("10.1.0.0/16", "10.1.0.0/16");
  expect_subnet ("  192.168.5.0/24 ", "192.168.5.0/24");
  expect_subnet ("10.2.0.0/32", "10.2.0.0/32");
  expect_subnet ("2001:DB8::/32", "2001:db8::/32");

  expect_bad_subnet ("");
  expect_bad_subnet ("10.1.0.0");
  expect_bad_subnet ("10.1.0.0/");
  expect_bad_subnet ("10.1.0.1/16");      /* host bits */
  expect_bad_subnet ("010.3.0.0/16");     /* leading zero */
  expect_bad_subnet ("0.0.0.0/0");
  expect_bad_subnet ("10.0.0.0/33");
  expect_bad_subnet ("10.0.0.0/x");
  expect_bad_subnet ("example/24");
}

/* Host bits get their own hint. */
static void
test_subnet_host_bits_hint (void)
{
  char *err = NULL;
  g_assert_null (sbv_subnet_normalise ("10.1.0.1/16", &err));
  g_assert_nonnull (strstr (err, "network address"));
  g_free (err);
}

/* Site names stay safe for DNS. */
static void
test_site_name_check (void)
{
  char *msg;
  g_assert_null (sbv_site_name_check ("Default-First-Site-Name"));
  g_assert_null (sbv_site_name_check ("Sydney2"));
  msg = sbv_site_name_check ("");               g_assert_nonnull (msg); g_free (msg);
  msg = sbv_site_name_check ("Sydney Office");  g_assert_nonnull (msg); g_free (msg);
  msg = sbv_site_name_check ("syd.example");    g_assert_nonnull (msg); g_free (msg);
  msg = sbv_site_name_check ("-Sydney");        g_assert_nonnull (msg); g_free (msg);
}

/* IPv4 sorts before IPv6, then by address bytes and prefix length. */
static void
test_subnet_compare (void)
{
  const char *names[] = { "2001:db8::/32", "10.10.0.0/16", "10.2.0.0/16",
                          "10.2.0.0/24", "192.168.1.0/24" };
  const char *want[]  = { "10.2.0.0/16", "10.2.0.0/24", "10.10.0.0/16",
                          "192.168.1.0/24", "2001:db8::/32" };
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_subnet_free);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    SbvSubnet *s = g_new0 (SbvSubnet, 1);
    s->name = g_strdup (names[i]);
    g_ptr_array_add (list, s);
  }
  g_ptr_array_sort (list, sbv_subnet_compare);
  for (guint i = 0; i < G_N_ELEMENTS (want); i++)
    g_assert_cmpstr (((SbvSubnet *) g_ptr_array_index (list, i))->name, ==, want[i]);
  g_ptr_array_unref (list);
}

/* A server object's site is two levels up. */
static void
test_site_dn_for_server (void)
{
  char *site = sbv_site_dn_for_server (
    "CN=DC1,CN=Servers,CN=Sydney,CN=Sites,CN=Configuration,DC=example,DC=com");
  g_assert_cmpstr (site, ==, "CN=Sydney,CN=Sites,CN=Configuration,DC=example,DC=com");
  g_free (site);
  g_assert_null (sbv_site_dn_for_server ("DC=com"));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sites/subnet-normalise",  test_subnet_normalise);
  g_test_add_func ("/sites/subnet-host-bits",  test_subnet_host_bits_hint);
  g_test_add_func ("/sites/site-name-check",   test_site_name_check);
  g_test_add_func ("/sites/subnet-compare",    test_subnet_compare);
  g_test_add_func ("/sites/site-dn-for-server", test_site_dn_for_server);
  return g_test_run ();
}
