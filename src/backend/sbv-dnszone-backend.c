#include "sbv-dnszone-backend.h"
#include "sbv-dn.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <string.h>

/* ── Types ──────────────────────────────────────────────────────────────── */

void
sbv_dns_zone_free (SbvDnsZone *zone)
{
  if (!zone) return;
  g_free (zone->name);
  g_free (zone->dn);
  g_free (zone->partition);
  g_free (zone);
}

void
sbv_dns_entry_free (SbvDnsEntry *entry)
{
  if (!entry) return;
  g_free (entry->name);
  g_free (entry->dn);
  sbv_dns_record_free (entry->record);
  g_free (entry);
}

/* ── Helpers (caller holds the LDAP lock) ───────────────────────────────── */

/* Returns the first value of `attr` on `entry`. Caller frees. */
static char *
first_value (LDAP *ld, LDAPMessage *entry, const char *attr)
{
  struct berval **bv  = ldap_get_values_len (ld, entry, attr);
  char           *out = (bv && bv[0]) ? g_strndup (bv[0]->bv_val, bv[0]->bv_len) : NULL;
  ldap_value_free_len (bv);
  return out;
}

/* TRUE when the entry's dNSTombstoned is TRUE. */
static gboolean
is_tombstoned (LDAP *ld, LDAPMessage *entry)
{
  char    *v = first_value (ld, entry, "dNSTombstoned");
  gboolean t = v && g_ascii_strcasecmp (v, "TRUE") == 0;
  g_free (v);
  return t;
}

/* Builds a GError from an LDAP result, adding the DC's diagnostic. */
static GError *
ldap_error (LDAP *ld, int rc, const char *what)
{
  const char *hint = NULL;
  switch (rc) {
    case LDAP_NO_SUCH_ATTRIBUTE:
    case LDAP_NO_SUCH_OBJECT:
      hint = "The record changed on the DC. Refresh and try again.";
      break;
    case LDAP_TYPE_OR_VALUE_EXISTS:
      hint = "That record already exists.";
      break;
    case LDAP_INSUFFICIENT_ACCESS:
      hint = "Access denied.";
      break;
    default:
      break;
  }
  char   *diag = NULL;
  ldap_get_option (ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &diag);
  GError *err  = g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "%s: %s%s%s%s", what,
                              hint ? hint : ldap_err2string (rc),
                              diag && *diag ? " (" : "",
                              diag && *diag ? diag : "",
                              diag && *diag ? ")" : "");
  if (diag) ldap_memfree (diag);
  return err;
}

/* Reads the node at `dn`. Returns LDAP_SUCCESS with `*records` set to a
 * GPtrArray of berval copies (as GBytes) of its dnsRecord values and
 * `*tombstoned` set, or LDAP_NO_SUCH_OBJECT. */
static int
read_node (LDAP *ld, const char *dn, GPtrArray **records, gboolean *tombstoned)
{
  char          *attrs[] = { (char *) "dnsRecord", (char *) "dNSTombstoned", NULL };
  struct timeval tv      = { 30, 0 };
  LDAPMessage   *res     = NULL;

  *records    = NULL;
  *tombstoned = FALSE;
  int rc = ldap_search_ext_s (ld, dn, LDAP_SCOPE_BASE, "(objectClass=*)",
                              attrs, 0, NULL, NULL, &tv, 1, &res);
  if (rc != LDAP_SUCCESS) {
    if (res) ldap_msgfree (res);
    return rc;
  }
  LDAPMessage *entry = ldap_first_entry (ld, res);
  if (!entry) {
    ldap_msgfree (res);
    return LDAP_NO_SUCH_OBJECT;
  }

  *tombstoned = is_tombstoned (ld, entry);
  *records    = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  struct berval **bv = ldap_get_values_len (ld, entry, "dnsRecord");
  for (int i = 0; bv && bv[i]; i++)
    g_ptr_array_add (*records, g_bytes_new (bv[i]->bv_val, bv[i]->bv_len));
  ldap_value_free_len (bv);
  ldap_msgfree (res);
  return LDAP_SUCCESS;
}

/* Finds the zone's SOA value on its "@" node. Returns a new ref, or NULL
 * when the zone has no well-formed SOA. */
static GBytes *
read_soa (LDAP *ld, const char *zone_dn)
{
  char      *apex = sbv_dn_build ("DC", "@", zone_dn);
  GPtrArray *recs = NULL;
  gboolean   tomb = FALSE;
  GBytes    *soa  = NULL;

  if (read_node (ld, apex, &recs, &tomb) == LDAP_SUCCESS) {
    for (guint i = 0; i < recs->len && !soa; i++) {
      GBytes       *b   = g_ptr_array_index (recs, i);
      gsize         len = 0;
      const guint8 *d   = g_bytes_get_data (b, &len);
      /* Only trust it if the whole record decodes, which checks the
       * layout before the serial is rewritten. */
      SbvDnsRecord *r = sbv_dns_record_parse (d, len);
      if (r && r->type == SBV_DNS_TYPE_SOA) soa = g_bytes_ref (b);
      sbv_dns_record_free (r);
    }
  }
  if (recs) g_ptr_array_unref (recs);
  g_free (apex);
  return soa;
}

/* Replaces the zone's SOA with one whose serial is `serial`. A failure
 * is only logged, since the record change itself has worked. */
static void
write_soa (LDAP *ld, const char *zone_dn, GBytes *old_soa, guint32 serial)
{
  GBytes *new_soa = sbv_dns_soa_with_serial (old_soa, serial);
  if (!new_soa) return;

  gsize          old_len = 0, new_len = 0;
  const guint8  *old_d   = g_bytes_get_data (old_soa, &old_len);
  const guint8  *new_d   = g_bytes_get_data (new_soa, &new_len);
  struct berval  old_bv  = { old_len, (char *) old_d };
  struct berval  new_bv  = { new_len, (char *) new_d };
  struct berval *old_v[] = { &old_bv, NULL };
  struct berval *new_v[] = { &new_bv, NULL };
  LDAPMod del = { LDAP_MOD_DELETE | LDAP_MOD_BVALUES, (char *) "dnsRecord", { .modv_bvals = old_v } };
  LDAPMod add = { LDAP_MOD_ADD    | LDAP_MOD_BVALUES, (char *) "dnsRecord", { .modv_bvals = new_v } };
  LDAPMod *mods[] = { &del, &add, NULL };

  char *apex = sbv_dn_build ("DC", "@", zone_dn);
  int   rc   = ldap_modify_ext_s (ld, apex, mods, NULL, NULL);
  if (rc != LDAP_SUCCESS)
    g_warning ("Couldn't raise the SOA serial of %s: %s", zone_dn, ldap_err2string (rc));
  g_free (apex);
  g_bytes_unref (new_soa);
}

/* ── Zones ──────────────────────────────────────────────────────────────── */

/* Adds the zones found one level under `container` to `zones`. A missing
 * container is skipped. */
static void
collect_zones (LDAP *ld, const char *container, const char *partition,
               GPtrArray *zones)
{
  char          *attrs[] = { (char *) "dc", NULL };
  struct timeval tv      = { 30, 0 };
  LDAPMessage   *res     = NULL;

  int rc = ldap_search_ext_s (ld, container, LDAP_SCOPE_ONELEVEL,
                              "(objectClass=dnsZone)", attrs, 0,
                              NULL, NULL, &tv, 0, &res);
  if (rc == LDAP_SUCCESS) {
    for (LDAPMessage *e = ldap_first_entry (ld, res); e; e = ldap_next_entry (ld, e)) {
      char *name = first_value (ld, e, "dc");
      if (!name || g_str_has_prefix (name, "..")
          || g_ascii_strcasecmp (name, "RootDNSServers") == 0) {
        g_free (name);
        continue;
      }
      char *dn = ldap_get_dn (ld, e);
      SbvDnsZone *z = g_new0 (SbvDnsZone, 1);
      z->name      = name;
      z->dn        = g_strdup (dn);
      z->partition = g_strdup (partition);
      ldap_memfree (dn);
      g_ptr_array_add (zones, z);
    }
  }
  if (res) ldap_msgfree (res);
}

/* Sorts forward zones before reverse ones, then by name. */
static gint
zone_cmp (gconstpointer a, gconstpointer b)
{
  const SbvDnsZone *za = *(SbvDnsZone * const *) a;
  const SbvDnsZone *zb = *(SbvDnsZone * const *) b;
  gboolean ra = g_str_has_suffix (za->name, ".arpa");
  gboolean rb = g_str_has_suffix (zb->name, ".arpa");
  if (ra != rb) return ra ? 1 : -1;
  return g_ascii_strcasecmp (za->name, zb->name);
}

/* Worker: reads the forest root from the root DSE, then lists zones in
 * all three places. Runs on a GTask thread. */
static void
zones_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  (void) task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  const char    *base    = sbv_connection_get_base_dn (conn);
  char          *forest  = NULL;
  char          *attrs[] = { (char *) "rootDomainNamingContext", NULL };
  struct timeval tv      = { 30, 0 };
  LDAPMessage   *res     = NULL;
  if (ldap_search_ext_s (ld, "", LDAP_SCOPE_BASE, "(objectClass=*)", attrs, 0,
                         NULL, NULL, &tv, 1, &res) == LDAP_SUCCESS) {
    LDAPMessage *e = ldap_first_entry (ld, res);
    if (e) forest = first_value (ld, e, "rootDomainNamingContext");
  }
  if (res) ldap_msgfree (res);
  if (!forest) forest = g_strdup (base);

  char *domain_c = g_strdup_printf ("CN=MicrosoftDNS,DC=DomainDnsZones,%s", base);
  char *forest_c = g_strdup_printf ("CN=MicrosoftDNS,DC=ForestDnsZones,%s", forest);
  char *legacy_c = g_strdup_printf ("CN=MicrosoftDNS,CN=System,%s", base);

  GPtrArray *zones = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_dns_zone_free);
  collect_zones (ld, domain_c, "Domain", zones);
  collect_zones (ld, forest_c, "Forest", zones);
  collect_zones (ld, legacy_c, "Legacy", zones);
  sbv_connection_release_ldap (conn);

  g_ptr_array_sort (zones, zone_cmp);
  g_free (domain_c);
  g_free (forest_c);
  g_free (legacy_c);
  g_free (forest);
  g_task_return_pointer (task, zones, (GDestroyNotify) g_ptr_array_unref);
}

void
sbv_dns_zones_list_async (SbvConnection *conn, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_run_in_thread (task, zones_thread);
  g_object_unref (task);
}

GPtrArray *
sbv_dns_zones_list_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Records ────────────────────────────────────────────────────────────── */

/* Sorts "@" first, then by name, then by type. */
static gint
entry_cmp (gconstpointer a, gconstpointer b)
{
  const SbvDnsEntry *ea = *(SbvDnsEntry * const *) a;
  const SbvDnsEntry *eb = *(SbvDnsEntry * const *) b;
  gboolean aa = strcmp (ea->name, "@") == 0;
  gboolean ab = strcmp (eb->name, "@") == 0;
  if (aa != ab) return aa ? -1 : 1;
  int c = g_ascii_strcasecmp (ea->name, eb->name);
  if (c) return c;
  return (int) ea->record->type - (int) eb->record->type;
}

/* Worker: lists the zone's dnsNode objects and decodes their records.
 * Runs on a GTask thread. */
static void
records_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  SbvConnection *conn    = SBV_CONNECTION (source);
  const char    *zone_dn = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  char          *attrs[] = { (char *) "dc", (char *) "dnsRecord",
                             (char *) "dNSTombstoned", NULL };
  struct timeval tv      = { 60, 0 };
  LDAPMessage   *res     = NULL;
  int rc = ldap_search_ext_s (ld, zone_dn, LDAP_SCOPE_ONELEVEL,
                              "(objectClass=dnsNode)", attrs, 0,
                              NULL, NULL, &tv, 0, &res);
  if (rc != LDAP_SUCCESS) {
    GError *err = ldap_error (ld, rc, "Couldn't list records");
    if (res) ldap_msgfree (res);
    sbv_connection_release_ldap (conn);
    g_task_return_error (task, err);
    return;
  }

  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) sbv_dns_entry_free);
  for (LDAPMessage *e = ldap_first_entry (ld, res); e; e = ldap_next_entry (ld, e)) {
    if (is_tombstoned (ld, e)) continue;
    char *name = first_value (ld, e, "dc");
    char *dn   = ldap_get_dn (ld, e);
    struct berval **bv = ldap_get_values_len (ld, e, "dnsRecord");
    for (int i = 0; bv && bv[i]; i++) {
      SbvDnsRecord *r = sbv_dns_record_parse ((const guint8 *) bv[i]->bv_val,
                                              bv[i]->bv_len);
      if (!r || r->type == SBV_DNS_TYPE_TOMBSTONE) {
        sbv_dns_record_free (r);
        continue;
      }
      SbvDnsEntry *en = g_new0 (SbvDnsEntry, 1);
      en->name   = g_strdup (name ? name : "?");
      en->dn     = g_strdup (dn);
      en->record = r;
      g_ptr_array_add (out, en);
    }
    ldap_value_free_len (bv);
    ldap_memfree (dn);
    g_free (name);
  }
  ldap_msgfree (res);
  sbv_connection_release_ldap (conn);

  g_ptr_array_sort (out, entry_cmp);
  g_task_return_pointer (task, out, (GDestroyNotify) g_ptr_array_unref);
}

void
sbv_dns_records_list_async (SbvConnection *conn, const char *zone_dn,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (zone_dn), g_free);
  g_task_run_in_thread (task, records_thread);
  g_object_unref (task);
}

GPtrArray *
sbv_dns_records_list_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Changes ────────────────────────────────────────────────────────────── */

typedef enum { CHANGE_ADD, CHANGE_UPDATE, CHANGE_DELETE } ChangeKind;

typedef struct {
  ChangeKind  kind;
  char       *zone_dn;
  char       *name;      /* add only */
  char       *node_dn;   /* update and delete */
  guint16     type;
  char       *value;
  guint32     ttl;
  GBytes     *old_raw;   /* update and delete */
} ChangeData;

/* Frees a change payload. */
static void
change_data_free (ChangeData *d)
{
  g_free (d->zone_dn);
  g_free (d->name);
  g_free (d->node_dn);
  g_free (d->value);
  if (d->old_raw) g_bytes_unref (d->old_raw);
  g_free (d);
}

/* Checks a new record against the name's live records: no exact
 * duplicate, and no CNAME next to anything else. Returns NULL when it
 * may be added. */
static GError *
check_conflicts (GPtrArray *existing, guint16 type, GBytes *candidate)
{
  gsize         clen = 0;
  const guint8 *cd   = g_bytes_get_data (candidate, &clen);
  SbvDnsRecord *cand = sbv_dns_record_parse (cd, clen);
  GError       *err  = NULL;

  for (guint i = 0; existing && i < existing->len && !err; i++) {
    GBytes       *b   = g_ptr_array_index (existing, i);
    gsize         len = 0;
    const guint8 *d   = g_bytes_get_data (b, &len);
    SbvDnsRecord *r   = sbv_dns_record_parse (d, len);
    if (!r || r->type == SBV_DNS_TYPE_TOMBSTONE) {
      sbv_dns_record_free (r);
      continue;
    }
    if (r->type == type && cand && g_ascii_strcasecmp (r->value, cand->value) == 0)
      err = g_error_new (SBV_ERROR, SBV_ERROR_LDAP, "That record already exists.");
    else if (type == SBV_DNS_TYPE_CNAME)
      err = g_error_new (SBV_ERROR, SBV_ERROR_LDAP,
                         "A CNAME can't share a name with other records.");
    else if (r->type == SBV_DNS_TYPE_CNAME)
      err = g_error_new (SBV_ERROR, SBV_ERROR_LDAP,
                         "This name has a CNAME, so it can't have other records.");
    sbv_dns_record_free (r);
  }
  sbv_dns_record_free (cand);
  return err;
}

/* Counts the live records in `values`. */
static guint
live_count (GPtrArray *values)
{
  guint n = 0;
  for (guint i = 0; values && i < values->len; i++) {
    gsize         len = 0;
    const guint8 *d   = g_bytes_get_data (g_ptr_array_index (values, i), &len);
    if (len >= 4 && (d[2] | (d[3] << 8)) != SBV_DNS_TYPE_TOMBSTONE) n++;
  }
  return n;
}

/* Adds the record in `value` to `node_dn`, creating the node or
 * reviving a deleted one as needed. */
static GError *
do_add (LDAP *ld, ChangeData *d, const char *node_dn, GBytes *value)
{
  GPtrArray *existing = NULL;
  gboolean   tomb     = FALSE;
  int        rc       = read_node (ld, node_dn, &existing, &tomb);
  if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_OBJECT)
    return ldap_error (ld, rc, "Couldn't read the name");

  if (rc == LDAP_SUCCESS && !tomb) {
    GError *err = check_conflicts (existing, d->type, value);
    if (err) {
      g_ptr_array_unref (existing);
      return err;
    }
  }
  if (existing) g_ptr_array_unref (existing);

  gsize          len    = 0;
  const guint8  *vd     = g_bytes_get_data (value, &len);
  struct berval  bv     = { len, (char *) vd };
  struct berval *bvals[] = { &bv, NULL };

  if (rc == LDAP_NO_SUCH_OBJECT) {
    char   *oc[]   = { (char *) "top", (char *) "dnsNode", NULL };
    LDAPMod oc_mod = { LDAP_MOD_ADD, (char *) "objectClass", { .modv_strvals = oc } };
    LDAPMod rec    = { LDAP_MOD_ADD | LDAP_MOD_BVALUES, (char *) "dnsRecord",
                       { .modv_bvals = bvals } };
    LDAPMod *mods[] = { &oc_mod, &rec, NULL };
    rc = ldap_add_ext_s (ld, node_dn, mods, NULL, NULL);
  } else if (tomb) {
    char   *no[]    = { (char *) "FALSE", NULL };
    LDAPMod rec     = { LDAP_MOD_REPLACE | LDAP_MOD_BVALUES, (char *) "dnsRecord",
                        { .modv_bvals = bvals } };
    LDAPMod tmod    = { LDAP_MOD_REPLACE, (char *) "dNSTombstoned", { .modv_strvals = no } };
    LDAPMod *mods[] = { &rec, &tmod, NULL };
    rc = ldap_modify_ext_s (ld, node_dn, mods, NULL, NULL);
  } else {
    LDAPMod rec     = { LDAP_MOD_ADD | LDAP_MOD_BVALUES, (char *) "dnsRecord",
                        { .modv_bvals = bvals } };
    LDAPMod *mods[] = { &rec, NULL };
    rc = ldap_modify_ext_s (ld, node_dn, mods, NULL, NULL);
  }
  return rc == LDAP_SUCCESS ? NULL : ldap_error (ld, rc, "Couldn't add the record");
}

/* Swaps the old value for the new one in a single modify, so it fails
 * cleanly if the old value has gone. */
static GError *
do_update (LDAP *ld, ChangeData *d, GBytes *value)
{
  gsize          olen = 0, nlen = 0;
  const guint8  *od   = g_bytes_get_data (d->old_raw, &olen);
  const guint8  *nd   = g_bytes_get_data (value, &nlen);
  struct berval  obv  = { olen, (char *) od };
  struct berval  nbv  = { nlen, (char *) nd };
  struct berval *ov[] = { &obv, NULL };
  struct berval *nv[] = { &nbv, NULL };
  LDAPMod del = { LDAP_MOD_DELETE | LDAP_MOD_BVALUES, (char *) "dnsRecord", { .modv_bvals = ov } };
  LDAPMod add = { LDAP_MOD_ADD    | LDAP_MOD_BVALUES, (char *) "dnsRecord", { .modv_bvals = nv } };
  LDAPMod *mods[] = { &del, &add, NULL };

  int rc = ldap_modify_ext_s (ld, d->node_dn, mods, NULL, NULL);
  return rc == LDAP_SUCCESS ? NULL : ldap_error (ld, rc, "Couldn't change the record");
}

/* Removes the old value, or the whole name when nothing else is left on
 * it and it isn't the zone itself. */
static GError *
do_delete (LDAP *ld, ChangeData *d)
{
  GPtrArray *existing = NULL;
  gboolean   tomb     = FALSE;
  int        rc       = read_node (ld, d->node_dn, &existing, &tomb);
  if (rc != LDAP_SUCCESS)
    return ldap_error (ld, rc, "Couldn't delete the record");

  char    *name    = sbv_dn_rdn_value (d->node_dn);
  gboolean is_apex = g_strcmp0 (name, "@") == 0;
  g_free (name);
  guint    live    = live_count (existing);
  g_ptr_array_unref (existing);

  if (live <= 1 && !is_apex) {
    rc = ldap_delete_ext_s (ld, d->node_dn, NULL, NULL);
  } else {
    gsize          olen = 0;
    const guint8  *od   = g_bytes_get_data (d->old_raw, &olen);
    struct berval  obv  = { olen, (char *) od };
    struct berval *ov[] = { &obv, NULL };
    LDAPMod del = { LDAP_MOD_DELETE | LDAP_MOD_BVALUES, (char *) "dnsRecord",
                    { .modv_bvals = ov } };
    LDAPMod *mods[] = { &del, NULL };
    rc = ldap_modify_ext_s (ld, d->node_dn, mods, NULL, NULL);
  }
  return rc == LDAP_SUCCESS ? NULL : ldap_error (ld, rc, "Couldn't delete the record");
}

/* Worker: applies one change, then raises the zone's SOA serial. The
 * new record carries the new serial. Runs on a GTask thread. */
static void
change_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  SbvConnection *conn = SBV_CONNECTION (source);
  ChangeData    *d    = task_data;
  (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (conn);
  if (!ld) {
    sbv_connection_release_ldap (conn);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION, "Not connected");
    return;
  }

  GBytes *soa    = read_soa (ld, d->zone_dn);
  guint32 serial = 0;
  if (soa) sbv_dns_soa_serial (soa, &serial);
  guint32 next   = serial + 1 ? serial + 1 : 1;

  GError *err   = NULL;
  GBytes *value = NULL;
  if (d->kind != CHANGE_DELETE)
    value = sbv_dns_record_build (d->type, d->value, d->ttl, next, &err);

  if (!err) {
    switch (d->kind) {
      case CHANGE_ADD: {
        char *node_dn = sbv_dn_build ("DC", d->name, d->zone_dn);
        err = do_add (ld, d, node_dn, value);
        g_free (node_dn);
        break;
      }
      case CHANGE_UPDATE:
        err = do_update (ld, d, value);
        break;
      case CHANGE_DELETE:
        err = do_delete (ld, d);
        break;
    }
  }
  if (!err && soa) write_soa (ld, d->zone_dn, soa, next);
  sbv_connection_release_ldap (conn);

  if (soa)   g_bytes_unref (soa);
  if (value) g_bytes_unref (value);
  if (err) g_task_return_error (task, err);
  else     g_task_return_boolean (task, TRUE);
}

/* Starts a change on a worker thread. Takes ownership of `d`. */
static void
start_change (SbvConnection *conn, ChangeData *d, GCancellable *cancellable,
              GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (conn, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) change_data_free);
  g_task_run_in_thread (task, change_thread);
  g_object_unref (task);
}

void
sbv_dns_record_add_async (SbvConnection *conn, const char *zone_dn,
                          const char *name, guint16 type, const char *value,
                          guint32 ttl, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  ChangeData *d = g_new0 (ChangeData, 1);
  d->kind    = CHANGE_ADD;
  d->zone_dn = g_strdup (zone_dn);
  d->name    = g_strdup (name);
  d->type    = type;
  d->value   = g_strdup (value);
  d->ttl     = ttl;
  start_change (conn, d, cancellable, callback, user_data);
}

void
sbv_dns_record_update_async (SbvConnection *conn, const char *zone_dn,
                             const char *node_dn, SbvDnsRecord *old,
                             const char *value, guint32 ttl,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  ChangeData *d = g_new0 (ChangeData, 1);
  d->kind    = CHANGE_UPDATE;
  d->zone_dn = g_strdup (zone_dn);
  d->node_dn = g_strdup (node_dn);
  d->type    = old->type;
  d->value   = g_strdup (value);
  d->ttl     = ttl;
  d->old_raw = g_bytes_ref (old->raw);
  start_change (conn, d, cancellable, callback, user_data);
}

void
sbv_dns_record_delete_async (SbvConnection *conn, const char *zone_dn,
                             const char *node_dn, SbvDnsRecord *old,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
  ChangeData *d = g_new0 (ChangeData, 1);
  d->kind    = CHANGE_DELETE;
  d->zone_dn = g_strdup (zone_dn);
  d->node_dn = g_strdup (node_dn);
  d->type    = old->type;
  d->old_raw = g_bytes_ref (old->raw);
  start_change (conn, d, cancellable, callback, user_data);
}

gboolean
sbv_dns_record_change_finish (SbvConnection *conn, GAsyncResult *result, GError **error)
{
  (void) conn;
  return g_task_propagate_boolean (G_TASK (result), error);
}
