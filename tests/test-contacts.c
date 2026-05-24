#include <glib.h>
#include "model/sbv-contact.h"

/* Defaults: every getter returns NULL on a freshly-allocated contact. */
static void
test_contact_defaults (void)
{
  SbvContact *c = sbv_contact_new ();
  g_assert_nonnull (c);
  g_assert_null (sbv_contact_get_dn               (c));
  g_assert_null (sbv_contact_get_cn               (c));
  g_assert_null (sbv_contact_get_display_name     (c));
  g_assert_null (sbv_contact_get_mail             (c));
  g_assert_null (sbv_contact_get_telephone_number (c));
  g_assert_null (sbv_contact_get_proxy_addresses  (c));
  g_assert_null (sbv_contact_get_ldap_attrs       (c));
  g_object_unref (c);
}

/* Each scalar setter persists, including overwrite-with-new-value. */
static void
test_contact_scalar_setters (void)
{
  SbvContact *c = sbv_contact_new ();

  sbv_contact_set_dn               (c, "CN=Bob,CN=Users,DC=example,DC=com");
  sbv_contact_set_cn               (c, "Bob Smith");
  sbv_contact_set_display_name     (c, "Bob Smith");
  sbv_contact_set_given_name       (c, "Bob");
  sbv_contact_set_sn               (c, "Smith");
  sbv_contact_set_mail             (c, "bob@example.com");
  sbv_contact_set_telephone_number (c, "+61 2 5550 0001");
  sbv_contact_set_mobile           (c, "+61 4 0000 0001");
  sbv_contact_set_fax              (c, "+61 2 5550 0099");
  sbv_contact_set_office           (c, "Sydney HQ");
  sbv_contact_set_street_address   (c, "1 Example St");
  sbv_contact_set_locality         (c, "Sydney");
  sbv_contact_set_state            (c, "NSW");
  sbv_contact_set_postal_code      (c, "2000");
  sbv_contact_set_country          (c, "AU");
  sbv_contact_set_description      (c, "Test contact");

  g_assert_cmpstr (sbv_contact_get_cn               (c), ==, "Bob Smith");
  g_assert_cmpstr (sbv_contact_get_mail             (c), ==, "bob@example.com");
  g_assert_cmpstr (sbv_contact_get_telephone_number (c), ==, "+61 2 5550 0001");
  g_assert_cmpstr (sbv_contact_get_locality         (c), ==, "Sydney");
  g_assert_cmpstr (sbv_contact_get_country          (c), ==, "AU");

  /* Overwrite */
  sbv_contact_set_mail (c, "bob.smith@example.com");
  g_assert_cmpstr (sbv_contact_get_mail (c), ==, "bob.smith@example.com");

  g_object_unref (c);
}

/* The proxy-addresses setter takes ownership of the GStrv and frees on
 * dispose; subsequent calls free the prior vector. */
static void
test_contact_proxy_addresses (void)
{
  SbvContact *c = sbv_contact_new ();

  char **first = g_new0 (char *, 3);
  first[0] = g_strdup ("SMTP:bob@example.com");
  first[1] = g_strdup ("smtp:bob.smith@example.com");
  first[2] = NULL;
  sbv_contact_set_proxy_addresses (c, first);

  const char *const *v = sbv_contact_get_proxy_addresses (c);
  g_assert_nonnull (v);
  g_assert_cmpstr (v[0], ==, "SMTP:bob@example.com");
  g_assert_cmpstr (v[1], ==, "smtp:bob.smith@example.com");
  g_assert_null   (v[2]);

  /* Replace with NULL clears. */
  sbv_contact_set_proxy_addresses (c, NULL);
  g_assert_null (sbv_contact_get_proxy_addresses (c));

  g_object_unref (c);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/contact/defaults",        test_contact_defaults);
  g_test_add_func ("/contact/scalar-setters",  test_contact_scalar_setters);
  g_test_add_func ("/contact/proxy-addresses", test_contact_proxy_addresses);
  return g_test_run ();
}
