#include "sbv-probe.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── SASL interact shim (see sbv-connection.c for rationale) ───────────── */

typedef struct {
  unsigned long  id;
  const char    *challenge;
  const char    *prompt;
  const char    *defresult;
  const void    *result;
  unsigned       len;
} SbvSaslInteract;

#define SBV_SASL_CB_LIST_END 0UL

/* GSSAPI bind callback — empties every prompt slot the SASL layer asks
 * about. AD's GSSAPI mech only consumes the credential cache, so there's
 * nothing to fill in. */
static int
gssapi_interact (LDAP *ld, unsigned flags, void *defaults, void *in)
{
  (void) ld; (void) flags; (void) defaults;
  SbvSaslInteract *interact = in;
  while (interact->id != SBV_SASL_CB_LIST_END) {
    interact->result = "";
    interact->len    = 0;
    interact++;
  }
  return LDAP_SUCCESS;
}

/* ── Result lifecycle ──────────────────────────────────────────────────── */

void
sbv_probe_result_free (SbvProbeResult *r)
{
  if (!r) return;
  if (r->rootdse)    g_hash_table_unref (r->rootdse);
  if (r->idmap)      sbv_idmap_hints_free (r->idmap);
  g_free (r->bind_error);
  g_free (r);
}

typedef struct {
  char       *host;
  int         port;
  gboolean    use_ldaps;
  gboolean    use_starttls;
  gboolean    skip_cert;
  SbvProfile *bind_profile;  /* optional, owned */
  char       *bind_password; /* optional, owned */
} ProbeData;

/* Frees the per-probe context. */
static void
probe_data_free (ProbeData *d)
{
  g_free (d->host);
  g_clear_object (&d->bind_profile);
  if (d->bind_password) {
    memset (d->bind_password, 0, strlen (d->bind_password));
    g_free (d->bind_password);
  }
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

/* Bind the supplied LDAP handle using the profile's auth mode. Returns
 * LDAP_SUCCESS on success, otherwise sets *err_out (caller frees) and
 * returns the LDAP error code. Doesn't unbind on failure — the caller
 * owns the handle either way. */
static int
do_bind (LDAP *ld, SbvProfile *profile, const char *password, char **err_out)
{
  int rc;
  if (sbv_profile_get_auth_type (profile) == SBV_AUTH_KERBEROS) {
    rc = ldap_sasl_interactive_bind_s (ld, NULL, "GSSAPI",
                                        NULL, NULL,
                                        LDAP_SASL_QUIET,
                                        gssapi_interact, NULL);
    if (rc != LDAP_SUCCESS && err_out)
      *err_out = g_strdup_printf ("GSSAPI bind failed: %s "
                                   "(hint: run kinit first)",
                                   ldap_err2string (rc));
  } else {
    struct berval cred = {
      .bv_val = (char *) password,
      .bv_len = password ? strlen (password) : 0,
    };
    rc = ldap_sasl_bind_s (ld, sbv_profile_get_bind_dn (profile),
                            LDAP_SASL_SIMPLE, &cred, NULL, NULL, NULL);
    if (rc != LDAP_SUCCESS && err_out)
      *err_out = g_strdup_printf ("Authentication failed: %s",
                                   ldap_err2string (rc));
  }
  return rc;
}

/* Reads the SFU30 idmap location once bound. Errors here are non-fatal —
 * the location is optional on Samba 4 — so the function always returns
 * an SbvIdmapHints (profile-derived, possibly with next_*_hint filled
 * from the DC). */
static SbvIdmapHints *
fetch_idmap (LDAP *ld, SbvProfile *profile)
{
  SbvIdmapHints *hints = sbv_idmap_hints_from_profile (profile);
  const char    *base  = sbv_profile_get_base_dn (profile);
  if (!base || !*base) return hints;

  char *yp_dn = g_strdup_printf (
    "CN=ypservers,CN=ypServ30,CN=RpcServices,CN=System,%s", base);

  const char *attrs[] = { "msSFU30MaxUidNumber", "msSFU30MaxGidNumber", NULL };
  LDAPMessage    *result = NULL;
  struct timeval  tv     = { 6, 0 };
  int rc = ldap_search_ext_s (ld, yp_dn, LDAP_SCOPE_BASE,
                               "(objectClass=*)", (char **) attrs, 0,
                               NULL, NULL, &tv, 1, &result);
  g_free (yp_dn);

  if (rc == LDAP_SUCCESS && result) {
    LDAPMessage *entry = ldap_first_entry (ld, result);
    if (entry) {
      struct berval **bv;
      bv = ldap_get_values_len (ld, entry, "msSFU30MaxUidNumber");
      if (bv && bv[0])
        hints->next_uid_hint = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
      ldap_value_free_len (bv);

      bv = ldap_get_values_len (ld, entry, "msSFU30MaxGidNumber");
      if (bv && bv[0])
        hints->next_gid_hint = g_ascii_strtoll (bv[0]->bv_val, NULL, 10);
      ldap_value_free_len (bv);

      if (hints->next_uid_hint >= 0 || hints->next_gid_hint >= 0) {
        g_free (hints->source);
        hints->source = g_strdup ("msSFU30 + profile config");
      }
    }
  }
  if (result) ldap_msgfree (result);
  return hints;
}

/* Worker: open a fresh LDAP handle, optionally STARTTLS, run a BASE search
 * against the empty DN to fetch the RootDSE, optionally bind and query
 * the SFU30 idmap range. The handle is unbound at the end whether or not
 * the search/bind succeeded. */
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

  SbvProbeResult *out = g_new0 (SbvProbeResult, 1);
  out->rootdse = raw;

  /* Optional bind + idmap query. Best-effort: bind failure surfaces in
   * `bind_error` and the rest of the probe still returns successfully. */
  if (d->bind_profile) {
    char *berr = NULL;
    int   brc  = do_bind (ld, d->bind_profile, d->bind_password, &berr);
    if (brc == LDAP_SUCCESS)
      out->idmap = fetch_idmap (ld, d->bind_profile);
    else
      out->bind_error = berr;
  }

  ldap_unbind_ext_s (ld, NULL, NULL);

  g_task_return_pointer (task, out, (GDestroyNotify) sbv_probe_result_free);
}

/* Public entry — see header. */
void
sbv_probe_dc_async (const char          *host,
                     int                  port,
                     gboolean             use_ldaps,
                     gboolean             use_starttls,
                     gboolean             skip_cert,
                     SbvProfile          *bind_profile,
                     const char          *bind_password,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GTask     *task = g_task_new (NULL, cancellable, callback, user_data);
  ProbeData *d    = g_new0 (ProbeData, 1);
  d->host          = g_strdup (host);
  d->port          = port > 0 ? port : (use_ldaps ? 636 : 389);
  d->use_ldaps     = use_ldaps;
  d->use_starttls  = use_starttls;
  d->skip_cert     = skip_cert;
  d->bind_profile  = bind_profile ? g_object_ref (bind_profile) : NULL;
  d->bind_password = g_strdup (bind_password);
  g_task_set_task_data (task, d, (GDestroyNotify) probe_data_free);
  g_task_run_in_thread (task, probe_thread);
  g_object_unref (task);
}

/* Returns the probe result on success, NULL on error. Caller owns. */
SbvProbeResult *
sbv_probe_dc_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
