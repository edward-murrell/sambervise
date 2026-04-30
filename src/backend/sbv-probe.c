#include "sbv-probe.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

typedef struct {
  char     *host;
  int       port;
  gboolean  use_ldaps;
  gboolean  use_starttls;
  gboolean  skip_cert;
} ProbeData;

/* Frees the per-probe context. */
static void
probe_data_free (ProbeData *d)
{
  g_free (d->host);
  g_free (d);
}

/* Reads every attribute of one entry into a fresh GHashTable<char*, GStrv>.
 * Mirrors the helper in sbv-ldap-browse.c — duplicated here to avoid a
 * cross-backend dependency. */
static GHashTable *
collect_attrs (LDAP *ld, LDAPMessage *entry)
{
  GHashTable *raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free,
                                            (GDestroyNotify) g_strfreev);
  BerElement *ber = NULL;
  for (char *aname = ldap_first_attribute (ld, entry, &ber);
       aname != NULL;
       aname = ldap_next_attribute (ld, entry, ber))
    {
      struct berval **av = ldap_get_values_len (ld, entry, aname);
      if (av) {
        int    cnt = ldap_count_values_len (av);
        char **sv  = g_new0 (char *, cnt + 1);
        for (int j = 0; j < cnt; j++) {
          sv[j] = g_utf8_validate (av[j]->bv_val, (gssize) av[j]->bv_len, NULL)
            ? g_strndup (av[j]->bv_val, av[j]->bv_len)
            : g_strdup_printf ("<binary %zu bytes>", av[j]->bv_len);
        }
        g_hash_table_insert (raw, g_strdup (aname), sv);
        ldap_value_free_len (av);
      }
      ldap_memfree (aname);
    }
  if (ber) ber_free (ber, 0);
  return raw;
}

/* Worker: open a fresh LDAP handle, optionally STARTTLS, run a BASE search
 * against the empty DN to fetch the RootDSE, and return the attrs. The
 * handle is unbound at the end whether or not the search succeeded. */
static void
probe_thread (GTask *task, gpointer source, gpointer task_data,
              GCancellable *cancellable)
{
  (void) source;
  (void) cancellable;
  ProbeData *d  = task_data;
  LDAP      *ld = NULL;
  int        rc;
  int        ver = LDAP_VERSION3;

  char *uri = d->use_ldaps
    ? g_strdup_printf ("ldaps://%s:%d", d->host, d->port)
    : g_strdup_printf ("ldap://%s:%d",  d->host, d->port);

  rc = ldap_initialize (&ld, uri);
  g_free (uri);
  if (rc != LDAP_SUCCESS) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "ldap_initialize: %s", ldap_err2string (rc));
    return;
  }

  ldap_set_option (ld, LDAP_OPT_PROTOCOL_VERSION, &ver);

  struct timeval timeout = { 6, 0 };
  ldap_set_option (ld, LDAP_OPT_NETWORK_TIMEOUT, &timeout);

  if (d->skip_cert) {
    int never = LDAP_OPT_X_TLS_NEVER;
    ldap_set_option (NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &never);
    ldap_set_option (ld,   LDAP_OPT_X_TLS_REQUIRE_CERT, &never);
  }

  if (d->use_starttls) {
    rc = ldap_start_tls_s (ld, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      ldap_unbind_ext_s (ld, NULL, NULL);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_TLS,
                               "STARTTLS failed: %s", ldap_err2string (rc));
      return;
    }
  }

  /* RootDSE read — anonymous, attrs limited to those a connect dialog
   * actually wants to surface. */
  char *attrs[] = {
    (char *) "defaultNamingContext",
    (char *) "dnsHostName",
    (char *) "supportedSASLMechanisms",
    (char *) "supportedLDAPVersion",
    (char *) "domainFunctionality",
    (char *) "forestFunctionality",
    (char *) "domainControllerFunctionality",
    (char *) "namingContexts",
    (char *) "currentTime",
    NULL
  };

  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 6, 0 };
  rc = ldap_search_ext_s (ld, "", LDAP_SCOPE_BASE,
                           "(objectClass=*)", attrs, 0,
                           NULL, NULL, &tv, 1, &result);
  if (rc != LDAP_SUCCESS) {
    if (result) ldap_msgfree (result);
    ldap_unbind_ext_s (ld, NULL, NULL);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "RootDSE read failed: %s", ldap_err2string (rc));
    return;
  }

  GHashTable  *raw   = NULL;
  LDAPMessage *entry = ldap_first_entry (ld, result);
  if (entry)
    raw = collect_attrs (ld, entry);
  else
    raw = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  g_free, (GDestroyNotify) g_strfreev);

  ldap_msgfree (result);
  ldap_unbind_ext_s (ld, NULL, NULL);

  g_task_return_pointer (task, raw, (GDestroyNotify) g_hash_table_unref);
}

/* Public entry — see header. */
void
sbv_probe_dc_async (const char          *host,
                     int                  port,
                     gboolean             use_ldaps,
                     gboolean             use_starttls,
                     gboolean             skip_cert,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GTask     *task = g_task_new (NULL, cancellable, callback, user_data);
  ProbeData *d    = g_new0 (ProbeData, 1);
  d->host         = g_strdup (host);
  d->port         = port > 0 ? port : (use_ldaps ? 636 : 389);
  d->use_ldaps    = use_ldaps;
  d->use_starttls = use_starttls;
  d->skip_cert    = skip_cert;
  g_task_set_task_data (task, d, (GDestroyNotify) probe_data_free);
  g_task_run_in_thread (task, probe_thread);
  g_object_unref (task);
}

/* Returns the RootDSE attribute table on success, NULL on error. */
GHashTable *
sbv_probe_dc_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
