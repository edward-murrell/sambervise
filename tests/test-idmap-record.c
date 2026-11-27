#include <glib.h>
#include "model/sbv-idmap-record.h"

/* Parses `kw` and checks it is valid. Caller frees. */
static SbvIdmapRecord *
parse_ok (const char * const *kw)
{
  char           *err = NULL;
  SbvIdmapRecord *r   = sbv_idmap_record_parse (kw, &err);
  g_assert_null (err);
  g_assert_nonnull (r);
  return r;
}

/* Checks `kw` is invalid with a message. */
static void
parse_bad (const char * const *kw)
{
  char           *err = NULL;
  SbvIdmapRecord *r   = sbv_idmap_record_parse (kw, &err);
  g_assert_null (r);
  g_assert_nonnull (err);
  g_free (err);
}

/* The spec's example reads back in full, ignoring other keywords. */
static void
test_example (void)
{
  const char *kw[] = {
    "hostadmin-idmap-version=1", "hostadmin-idmap-backend=ad",
    "hostadmin-idmap-range=10000-1999999", "hostadmin-idmap-source=rfc2307",
    "hostadmin-idmap-written-by=files1.ad.example.com",
    "hostadmin-idmap-written=2026-11-27", "unrelated-keyword",
    "hostadmin-idmap-future-setting=yes", NULL };
  SbvIdmapRecord *r = parse_ok (kw);
  g_assert_cmpint (r->version, ==, 1);
  g_assert_cmpint (r->backend, ==, SBV_IDMAP_BACKEND_AD);
  g_assert_cmpint (r->low, ==, 10000);
  g_assert_cmpint (r->high, ==, 1999999);
  g_assert_cmpstr (r->source, ==, "rfc2307");
  g_assert_cmpstr (r->written_by, ==, "files1.ad.example.com");
  g_assert_cmpstr (r->written, ==, "2026-11-27");
  sbv_idmap_record_free (r);
}

/* With no range, the default applies, and rid maps low + RID. */
static void
test_rid_default_range (void)
{
  const char *kw[] = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=rid", NULL };
  SbvIdmapRecord *r = parse_ok (kw);
  g_assert_cmpint (r->low, ==, 10000);
  g_assert_cmpint (r->high, ==, 999999);
  g_assert_cmpint (sbv_idmap_record_rid_to_id (r, 1104), ==, 11104);
  g_assert_cmpint (sbv_idmap_record_rid_to_id (r, 513), ==, 10513);
  g_assert_cmpint (sbv_idmap_record_rid_to_id (r, 989999), ==, 999999);
  g_assert_cmpint (sbv_idmap_record_rid_to_id (r, 990000), ==, -1);
  g_assert_cmpint (sbv_idmap_record_rid_to_id (r, -1), ==, -1);
  sbv_idmap_record_free (r);
}

/* Every invalid case in the spec is refused. */
static void
test_invalid (void)
{
  const char *no_version[]  = { "hostadmin-idmap-backend=rid", NULL };
  const char *bad_version[] = { "hostadmin-idmap-version=one", "hostadmin-idmap-backend=rid", NULL };
  const char *zero[]        = { "hostadmin-idmap-version=0", "hostadmin-idmap-backend=rid", NULL };
  const char *future[]      = { "hostadmin-idmap-version=2", "hostadmin-idmap-backend=rid", NULL };
  const char *dup[]         = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=rid",
                                "hostadmin-idmap-backend=ad", NULL };
  const char *no_backend[]  = { "hostadmin-idmap-version=1", NULL };
  const char *bad_backend[] = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=tdb", NULL };
  const char *bad_range[]   = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=ad",
                                "hostadmin-idmap-range=10000", NULL };
  const char *flat_range[]  = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=ad",
                                "hostadmin-idmap-range=5000-5000", NULL };
  const char *neg_range[]   = { "hostadmin-idmap-version=1", "hostadmin-idmap-backend=ad",
                                "hostadmin-idmap-range=-5-100", NULL };
  parse_bad (no_version);
  parse_bad (bad_version);
  parse_bad (zero);
  parse_bad (future);
  parse_bad (dup);
  parse_bad (no_backend);
  parse_bad (bad_backend);
  parse_bad (bad_range);
  parse_bad (flat_range);
  parse_bad (neg_range);
  parse_bad (NULL);
}

/* The RID is the last sub-authority, little-endian. */
static void
test_sid_rid (void)
{
  /* S-1-5-21-1-2-3-1104 */
  const guchar sid[] = { 1, 5, 0, 0, 0, 0, 0, 5,
                         21, 0, 0, 0,  1, 0, 0, 0,  2, 0, 0, 0,  3, 0, 0, 0,
                         0x50, 0x04, 0, 0 };
  g_assert_cmpint (sbv_sid_rid (sid, sizeof sid), ==, 1104);
  g_assert_cmpint (sbv_sid_rid (sid, sizeof sid - 1), ==, -1);
  g_assert_cmpint (sbv_sid_rid (NULL, 0), ==, -1);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/idmap-record/example",            test_example);
  g_test_add_func ("/idmap-record/rid-default-range",  test_rid_default_range);
  g_test_add_func ("/idmap-record/invalid",            test_invalid);
  g_test_add_func ("/idmap-record/sid-rid",            test_sid_rid);
  return g_test_run ();
}
