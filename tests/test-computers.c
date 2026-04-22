#include <glib.h>
#include "model/sbv-computer.h"

static void
test_computer_properties (void)
{
  SbvComputer *c = sbv_computer_new ();

  g_assert_nonnull (c);
  g_assert_null (sbv_computer_get_dn (c));
  g_assert_true (sbv_computer_get_enabled (c));
  g_assert_cmpuint (sbv_computer_get_spn_count (c), ==, 0);

  sbv_computer_set_dn  (c, "CN=PC01,CN=Computers,DC=example,DC=com");
  sbv_computer_set_sam (c, "PC01$");
  sbv_computer_set_cn  (c, "PC01");
  sbv_computer_set_dns_hostname (c, "pc01.example.com");
  sbv_computer_set_os           (c, "Windows 10 Pro");
  sbv_computer_set_os_version   (c, "10.0 (19045)");
  sbv_computer_set_description  (c, "Marketing workstation");

  g_assert_cmpstr (sbv_computer_get_sam (c),          ==, "PC01$");
  g_assert_cmpstr (sbv_computer_get_cn  (c),          ==, "PC01");
  g_assert_cmpstr (sbv_computer_get_dns_hostname (c), ==, "pc01.example.com");
  g_assert_cmpstr (sbv_computer_get_os (c),           ==, "Windows 10 Pro");
  g_assert_cmpstr (sbv_computer_get_os_version (c),   ==, "10.0 (19045)");
  g_assert_cmpstr (sbv_computer_get_description (c),  ==, "Marketing workstation");

  g_object_unref (c);
}

static void
test_computer_enabled_uac (void)
{
  SbvComputer *c = sbv_computer_new ();

  /* default enabled */
  g_assert_true (sbv_computer_get_enabled (c));

  sbv_computer_set_enabled (c, FALSE);
  g_assert_false (sbv_computer_get_enabled (c));

  sbv_computer_set_uac (c, 4096); /* WORKSTATION_TRUST_ACCOUNT */
  g_assert_cmpint (sbv_computer_get_uac (c), ==, 4096);

  g_object_unref (c);
}

static void
test_computer_spn (void)
{
  SbvComputer *c = sbv_computer_new ();

  g_assert_cmpuint (sbv_computer_get_spn_count (c), ==, 0);
  g_assert_null    (sbv_computer_get_spn (c));

  char **spns = g_new0 (char *, 3);
  spns[0] = g_strdup ("HOST/pc01.example.com");
  spns[1] = g_strdup ("HOST/PC01");
  spns[2] = NULL;
  sbv_computer_set_spn (c, spns);  /* takes ownership */

  g_assert_cmpuint (sbv_computer_get_spn_count (c), ==, 2);
  const char * const *got = sbv_computer_get_spn (c);
  g_assert_cmpstr (got[0], ==, "HOST/pc01.example.com");
  g_assert_cmpstr (got[1], ==, "HOST/PC01");
  g_assert_null   (got[2]);

  /* replacing frees the old array */
  char **empty = g_new0 (char *, 1);
  empty[0] = NULL;
  sbv_computer_set_spn (c, empty);
  g_assert_cmpuint (sbv_computer_get_spn_count (c), ==, 0);

  g_object_unref (c);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/computer/properties",  test_computer_properties);
  g_test_add_func ("/computer/enabled_uac", test_computer_enabled_uac);
  g_test_add_func ("/computer/spn",         test_computer_spn);
  return g_test_run ();
}
