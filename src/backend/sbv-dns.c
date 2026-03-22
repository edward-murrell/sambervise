#include "sbv-dns.h"

/* GResolver does not expose GSrvTarget as a GObject, so we store results
 * in a simple GObject wrapper so we can put them in a GListStore. */

/* ── SbvDcTarget: thin GObject wrapping a single SRV result ─────────────── */

#define SBV_TYPE_DC_TARGET (sbv_dc_target_get_type ())
G_DECLARE_FINAL_TYPE (SbvDcTarget, sbv_dc_target, SBV, DC_TARGET, GObject)

struct _SbvDcTarget {
  GObject  parent;
  char    *hostname;
  guint16  port;
  guint16  priority;
  guint16  weight;
};

G_DEFINE_TYPE (SbvDcTarget, sbv_dc_target, G_TYPE_OBJECT)

static void
sbv_dc_target_finalize (GObject *obj)
{
  g_free (SBV_DC_TARGET (obj)->hostname);
  G_OBJECT_CLASS (sbv_dc_target_parent_class)->finalize (obj);
}

static void sbv_dc_target_class_init (SbvDcTargetClass *k) { G_OBJECT_CLASS (k)->finalize = sbv_dc_target_finalize; }
static void sbv_dc_target_init       (SbvDcTarget *self)    { (void) self; }

static SbvDcTarget *
sbv_dc_target_from_srv (GSrvTarget *srv)
{
  SbvDcTarget *t = g_object_new (SBV_TYPE_DC_TARGET, NULL);
  t->hostname    = g_strdup (g_srv_target_get_hostname (srv));
  t->port        = g_srv_target_get_port (srv);
  t->priority    = g_srv_target_get_priority (srv);
  t->weight      = g_srv_target_get_weight (srv);
  /* Expose fields as GObject data so callers don't need the private type */
  g_object_set_data_full (G_OBJECT (t), "hostname", g_strdup (t->hostname), g_free);
  g_object_set_data      (G_OBJECT (t), "port",     GUINT_TO_POINTER ((guint) t->port));
  return t;
}

const char *sbv_dc_target_get_hostname (SbvDcTarget *t) { return t->hostname; }
guint16     sbv_dc_target_get_port     (SbvDcTarget *t) { return t->port; }

/* ── DNS lookup thread ──────────────────────────────────────────────────── */

typedef struct {
  char *domain;
} DnsData;

static void
dns_data_free (DnsData *d)
{
  g_free (d->domain);
  g_free (d);
}

static void
discover_thread (GTask *task, gpointer source_object, gpointer task_data,
                 GCancellable *cancellable)
{
  (void) source_object;
  DnsData    *d        = task_data;
  GResolver  *resolver = g_resolver_get_default ();
  GListStore *store    = g_list_store_new (SBV_TYPE_DC_TARGET);
  GError     *err      = NULL;

  /* Prefer the MS-DNS msdcs sub-domain: _ldap._tcp.dc._msdcs.<domain> */
  char  *msdcs   = g_strdup_printf ("dc._msdcs.%s", d->domain);
  GList *results = g_resolver_lookup_service (resolver, "ldap", "tcp",
                                               msdcs, cancellable, &err);
  g_free (msdcs);

  if (!results || err) {
    g_clear_error (&err);
    /* Fallback to standard _ldap._tcp.<domain> */
    results = g_resolver_lookup_service (resolver, "ldap", "tcp",
                                          d->domain, cancellable, &err);
  }

  if (err) {
    g_task_return_error (task, err);
    g_object_unref (store);
    g_object_unref (resolver);
    return;
  }

  /* g_resolver_lookup_service returns the list already sorted by RFC 2782
   * priority/weight ordering. */
  for (GList *l = results; l; l = l->next) {
    SbvDcTarget *t = sbv_dc_target_from_srv (l->data);
    g_list_store_append (store, t);
    g_object_unref (t);
  }

  g_resolver_free_targets (results);
  g_object_unref (resolver);

  if (g_list_model_get_n_items (G_LIST_MODEL (store)) == 0) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "No LDAP SRV records found for domain '%s'",
                              d->domain);
    g_object_unref (store);
    return;
  }

  g_task_return_pointer (task, store, g_object_unref);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void
sbv_dns_discover_async (const char          *domain,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  DnsData *d   = g_new0 (DnsData, 1);
  d->domain    = g_strdup (domain);

  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) dns_data_free);
  g_task_run_in_thread (task, discover_thread);
  g_object_unref (task);
}

GListStore *
sbv_dns_discover_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
