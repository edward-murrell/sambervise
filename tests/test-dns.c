#include <glib.h>
#include <string.h>
#include "model/sbv-dns-record.h"

/* Builds a record, then checks the header bytes and that it decodes. */
static void
test_a_round_trip (void)
{
  GError *err = NULL;
  GBytes *b   = sbv_dns_record_build (SBV_DNS_TYPE_A, "192.0.2.10", 900, 7, &err);
  g_assert_no_error (err);

  gsize         len = 0;
  const guint8 *d   = g_bytes_get_data (b, &len);
  g_assert_cmpuint (len, ==, 28);
  g_assert_cmpuint (d[0], ==, 4);       /* data length, LE */
  g_assert_cmpuint (d[2], ==, 1);       /* type A, LE */
  g_assert_cmpuint (d[4], ==, 5);       /* version */
  g_assert_cmpuint (d[5], ==, 0xF0);    /* rank */
  g_assert_cmpuint (d[8], ==, 7);       /* serial, LE */
  g_assert_cmpuint (d[14], ==, 0x03);   /* TTL 900 = 0x384, BE */
  g_assert_cmpuint (d[15], ==, 0x84);
  g_assert_cmpuint (d[24], ==, 192);

  SbvDnsRecord *r = sbv_dns_record_parse (d, len);
  g_assert_nonnull (r);
  g_assert_cmpuint (r->type, ==, SBV_DNS_TYPE_A);
  g_assert_cmpuint (r->ttl, ==, 900);
  g_assert_cmpuint (r->serial, ==, 7);
  g_assert_cmpstr (r->value, ==, "192.0.2.10");
  sbv_dns_record_free (r);
  g_bytes_unref (b);

  b = sbv_dns_record_build (SBV_DNS_TYPE_AAAA, "2001:db8::10", 300, 1, &err);
  g_assert_no_error (err);
  d = g_bytes_get_data (b, &len);
  r = sbv_dns_record_parse (d, len);
  g_assert_cmpstr (r->value, ==, "2001:db8::10");
  sbv_dns_record_free (r);
  g_bytes_unref (b);

  g_assert_null (sbv_dns_record_build (SBV_DNS_TYPE_A, "300.1.1.1", 900, 1, &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
}

/* Names use the count-name layout: total length, label count, labels
 * and a zero byte. A trailing dot is dropped. */
static void
test_name_round_trip (void)
{
  GError *err = NULL;
  GBytes *b   = sbv_dns_record_build (SBV_DNS_TYPE_CNAME, "www.example.com.",
                                      900, 1, &err);
  g_assert_no_error (err);

  gsize         len = 0;
  const guint8 *d   = g_bytes_get_data (b, &len);
  const guint8  want[] = { 17, 3, 3, 'w','w','w', 7, 'e','x','a','m','p','l','e',
                           3, 'c','o','m', 0 };
  g_assert_cmpuint (len, ==, 24 + sizeof want);
  g_assert_cmpmem (d + 24, sizeof want, want, sizeof want);

  SbvDnsRecord *r = sbv_dns_record_parse (d, len);
  g_assert_cmpstr (r->value, ==, "www.example.com");
  sbv_dns_record_free (r);
  g_bytes_unref (b);

  g_assert_null (sbv_dns_record_build (SBV_DNS_TYPE_PTR, "bad..name", 900, 1, &err));
  g_clear_error (&err);
}

/* Appends a count name to a test buffer. */
static void
push_name (GByteArray *a, const char *const *labels)
{
  guint8 count = 0, total = 1;
  for (const char *const *l = labels; *l; l++) { count++; total += 1 + strlen (*l); }
  g_byte_array_append (a, &total, 1);
  g_byte_array_append (a, &count, 1);
  for (const char *const *l = labels; *l; l++) {
    guint8 n = strlen (*l);
    g_byte_array_append (a, &n, 1);
    g_byte_array_append (a, (const guint8 *) *l, n);
  }
  guint8 z = 0;
  g_byte_array_append (a, &z, 1);
}

/* Builds an SOA record by hand: serial 0x78a42b35, big-endian timers. */
static GBytes *
make_soa (void)
{
  GByteArray *a = g_byte_array_new ();
  guint8 hdr[24] = { 0, 0, 6, 0, 5, 0xF0, 0, 0,  0x35, 0x2b, 0xa4, 0x78,
                     0, 0, 0x0e, 0x10,  0, 0, 0, 0,  0, 0, 0, 0 };
  g_byte_array_append (a, hdr, 24);
  guint8 timers[20] = { 0x78, 0xa4, 0x2b, 0x35,  0, 0, 0x03, 0x84,
                        0, 0, 0x02, 0x58,  0, 0x01, 0x51, 0x80,
                        0, 0, 0x0e, 0x10 };
  g_byte_array_append (a, timers, 20);
  const char *mname[] = { "dc1", "example", "com", NULL };
  const char *rname[] = { "hostmaster", "example", "com", NULL };
  push_name (a, mname);
  push_name (a, rname);
  guint16 dl = a->len - 24;
  a->data[0] = dl & 0xff;
  a->data[1] = dl >> 8;
  return g_byte_array_free_to_bytes (a);
}

/* Reads the SOA serial, and bumps it in both places. */
static void
test_soa (void)
{
  GBytes  *soa    = make_soa ();
  guint32  serial = 0;
  g_assert_true (sbv_dns_soa_serial (soa, &serial));
  g_assert_cmpuint (serial, ==, 0x78a42b35);

  gsize         len  = 0;
  const guint8 *data = g_bytes_get_data (soa, &len);
  SbvDnsRecord *r    = sbv_dns_record_parse (data, len);
  g_assert_nonnull (r);
  g_assert_cmpuint (r->ttl, ==, 3600);
  g_assert_true (g_str_has_prefix (r->value, "dc1.example.com hostmaster.example.com serial 2024024885"));
  sbv_dns_record_free (r);

  GBytes *bumped = sbv_dns_soa_with_serial (soa, serial + 1);
  guint32 again  = 0;
  g_assert_true (sbv_dns_soa_serial (bumped, &again));
  g_assert_cmpuint (again, ==, serial + 1);
  const guint8 *d = g_bytes_get_data (bumped, &len);
  g_assert_cmpuint (d[8], ==, 0x36);     /* header serial, LE */
  g_bytes_unref (bumped);
  g_bytes_unref (soa);
}

/* Record names: labels of 1 to 63 bytes, "@" only when allowed. */
static void
test_names (void)
{
  g_assert_true  (sbv_dns_name_valid ("host", FALSE));
  g_assert_true  (sbv_dns_name_valid ("a.b.c.", FALSE));
  g_assert_true  (sbv_dns_name_valid ("_ldap._tcp", FALSE));
  g_assert_true  (sbv_dns_name_valid ("@", TRUE));
  g_assert_false (sbv_dns_name_valid ("@", FALSE));
  g_assert_false (sbv_dns_name_valid ("", FALSE));
  g_assert_false (sbv_dns_name_valid ("a..b", FALSE));
  g_assert_false (sbv_dns_name_valid ("has space", FALSE));
  char *long_label = g_strnfill (64, 'x');
  g_assert_false (sbv_dns_name_valid (long_label, FALSE));
  g_free (long_label);
}

/* Short or truncated values don't decode. */
static void
test_malformed (void)
{
  guint8 shortbuf[10] = { 0 };
  g_assert_null (sbv_dns_record_parse (shortbuf, sizeof shortbuf));
  guint8 trunc[24] = { 8, 0, 1, 0 };   /* claims 8 data bytes, has none */
  g_assert_null (sbv_dns_record_parse (trunc, sizeof trunc));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dns/a-round-trip",    test_a_round_trip);
  g_test_add_func ("/dns/name-round-trip", test_name_round_trip);
  g_test_add_func ("/dns/soa",             test_soa);
  g_test_add_func ("/dns/names",           test_names);
  g_test_add_func ("/dns/malformed",       test_malformed);
  return g_test_run ();
}
