#include <glib.h>
#include "backend/sbv-dn.h"

/* Special characters, and leading or trailing spaces and '#', get a
 * backslash. Plain text passes through. */
static void
test_dn_escape (void)
{
  char *s;

  s = sbv_dn_escape_value ("Sales");
  g_assert_cmpstr (s, ==, "Sales");
  g_free (s);

  s = sbv_dn_escape_value ("Smith, Bob");
  g_assert_cmpstr (s, ==, "Smith\\, Bob");
  g_free (s);

  s = sbv_dn_escape_value ("a+b=c<d>;\"e\\");
  g_assert_cmpstr (s, ==, "a\\+b\\=c\\<d\\>\\;\\\"e\\\\");
  g_free (s);

  s = sbv_dn_escape_value (" #lead trail ");
  g_assert_cmpstr (s, ==, "\\ #lead trail\\ ");
  g_free (s);

  s = sbv_dn_escape_value ("#tag");
  g_assert_cmpstr (s, ==, "\\#tag");
  g_free (s);
}

/* Splits at the first comma that isn't escaped. */
static void
test_dn_split (void)
{
  char *rdn = NULL, *parent = NULL;

  g_assert_true (sbv_dn_split ("OU=Sales,DC=example,DC=com", &rdn, &parent));
  g_assert_cmpstr (rdn,    ==, "OU=Sales");
  g_assert_cmpstr (parent, ==, "DC=example,DC=com");
  g_free (rdn); g_free (parent);

  g_assert_true (sbv_dn_split ("CN=Smith\\, Bob,OU=Staff,DC=example,DC=com", &rdn, &parent));
  g_assert_cmpstr (rdn,    ==, "CN=Smith\\, Bob");
  g_assert_cmpstr (parent, ==, "OU=Staff,DC=example,DC=com");
  g_free (rdn); g_free (parent);

  /* A trailing escaped backslash doesn't hide the following comma. */
  g_assert_true (sbv_dn_split ("CN=back\\\\,DC=com", &rdn, &parent));
  g_assert_cmpstr (rdn,    ==, "CN=back\\\\");
  g_assert_cmpstr (parent, ==, "DC=com");
  g_free (rdn); g_free (parent);

  g_assert_false (sbv_dn_split ("DC=com", &rdn, &parent));
  g_assert_null (rdn);
  g_assert_null (parent);

  g_assert_false (sbv_dn_split (NULL, NULL, NULL));
}

/* Returns the leading RDN value with escapes removed, including hex pairs. */
static void
test_dn_rdn_value (void)
{
  char *v;

  v = sbv_dn_rdn_value ("CN=Smith\\, Bob,OU=Staff,DC=example,DC=com");
  g_assert_cmpstr (v, ==, "Smith, Bob");
  g_free (v);

  v = sbv_dn_rdn_value ("OU=R\\2CD,DC=example,DC=com");
  g_assert_cmpstr (v, ==, "R,D");
  g_free (v);

  v = sbv_dn_rdn_value ("DC=com");
  g_assert_cmpstr (v, ==, "com");
  g_free (v);

  g_assert_null (sbv_dn_rdn_value ("garbage"));
}

/* Ancestry is suffix-based, case-insensitive, and respects RDN boundaries. */
static void
test_dn_is_within (void)
{
  const char *base = "DC=example,DC=com";

  g_assert_true  (sbv_dn_is_within ("DC=example,DC=com", base));
  g_assert_true  (sbv_dn_is_within ("OU=Sales,DC=example,DC=com", base));
  g_assert_true  (sbv_dn_is_within ("ou=sales,dc=EXAMPLE,dc=com", base));
  g_assert_false (sbv_dn_is_within ("DC=other,DC=com", base));
  g_assert_false (sbv_dn_is_within ("DC=com", base));

  g_assert_true  (sbv_dn_is_within ("OU=Syd,OU=Sales,DC=example,DC=com",
                                    "OU=Sales,DC=example,DC=com"));
  g_assert_false (sbv_dn_is_within ("OU=XSales,DC=example,DC=com",
                                    "Sales,DC=example,DC=com"));
  /* "OU=a\,OU=Sales,..." is one RDN whose value contains a comma. */
  g_assert_false (sbv_dn_is_within ("OU=a\\,OU=Sales,DC=example,DC=com",
                                    "OU=Sales,DC=example,DC=com"));
  g_assert_false (sbv_dn_is_within (NULL, base));
  g_assert_false (sbv_dn_is_within ("OU=x", ""));
}

/* Equality ignores ASCII case. */
static void
test_dn_equal (void)
{
  g_assert_true  (sbv_dn_equal ("CN=Users,DC=x", "cn=users,dc=X"));
  g_assert_false (sbv_dn_equal ("CN=Users,DC=x", "CN=Computers,DC=x"));
  g_assert_true  (sbv_dn_equal (NULL, NULL));
  g_assert_false (sbv_dn_equal ("CN=a", NULL));
}

/* DC-only DNs become dotted domain names; anything else is copied. */
static void
test_dn_domain_name (void)
{
  char *d;

  d = sbv_dn_domain_name ("DC=example,DC=com");
  g_assert_cmpstr (d, ==, "example.com");
  g_free (d);

  d = sbv_dn_domain_name ("dc=corp, dc=example, dc=com");
  g_assert_cmpstr (d, ==, "corp.example.com");
  g_free (d);

  d = sbv_dn_domain_name ("OU=x,DC=com");
  g_assert_cmpstr (d, ==, "OU=x,DC=com");
  g_free (d);
}

/* Path runs from the top-level OU down; empty at the base; NULL outside. */
static void
test_dn_path_components (void)
{
  const char *base = "DC=example,DC=com";
  char **p;

  p = sbv_dn_path_components ("OU=Sydney,OU=Sales,DC=example,DC=com", base);
  g_assert_nonnull (p);
  g_assert_cmpuint (g_strv_length (p), ==, 2);
  g_assert_cmpstr (p[0], ==, "Sales");
  g_assert_cmpstr (p[1], ==, "Sydney");
  g_strfreev (p);

  p = sbv_dn_path_components ("CN=Smith\\, Bob,CN=Users,DC=example,DC=com", base);
  g_assert_cmpuint (g_strv_length (p), ==, 2);
  g_assert_cmpstr (p[0], ==, "Users");
  g_assert_cmpstr (p[1], ==, "Smith, Bob");
  g_strfreev (p);

  p = sbv_dn_path_components (base, base);
  g_assert_nonnull (p);
  g_assert_cmpuint (g_strv_length (p), ==, 0);
  g_strfreev (p);

  g_assert_null (sbv_dn_path_components ("DC=other,DC=com", base));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dn/escape",           test_dn_escape);
  g_test_add_func ("/dn/split",            test_dn_split);
  g_test_add_func ("/dn/rdn-value",        test_dn_rdn_value);
  g_test_add_func ("/dn/is-within",        test_dn_is_within);
  g_test_add_func ("/dn/equal",            test_dn_equal);
  g_test_add_func ("/dn/domain-name",      test_dn_domain_name);
  g_test_add_func ("/dn/path-components",  test_dn_path_components);
  return g_test_run ();
}
