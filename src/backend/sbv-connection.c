#include "sbv-connection.h"
#include "../sbv-error.h"

#include <ldap.h>
#include <lber.h>
#include <sys/time.h>
#include <string.h>

/* ── Minimal SASL interact struct (matches <sasl/sasl.h> sasl_interact_t)
 *    We define it ourselves to avoid a build-time dependency on libsasl2-dev.
 *    The layout is fixed by the Cyrus SASL ABI and does not vary.          */
typedef struct {
  unsigned long  id;          /* SASL_CB_LIST_END == 0UL when done   */
  const char    *challenge;
  const char    *prompt;
  const char    *defresult;
  const void    *result;      /* set to "" for GSSAPI (no prompts)   */
  unsigned       len;
} SbvSaslInteract;

#define SBV_SASL_CB_LIST_END 0UL

struct _SbvConnection {
  GObject     parent;

  GMutex      mutex;
  LDAP       *ld;

  char       *host;
  char       *base_dn;
  int         port;
  gboolean    connected;
  SbvProfile *profile;   /* owned ref to the profile we're bound with     */
};

G_DEFINE_TYPE (SbvConnection, sbv_connection, G_TYPE_OBJECT)

/* ── GObject lifecycle ─────────────────────────────────────────────────── */

static void
sbv_connection_finalize (GObject *object)
{
  SbvConnection *self = SBV_CONNECTION (object);
  sbv_connection_disconnect (self);
  g_mutex_clear (&self->mutex);
  g_free (self->host);
  g_free (self->base_dn);
  g_clear_object (&self->profile);
  G_OBJECT_CLASS (sbv_connection_parent_class)->finalize (object);
}

static void
sbv_connection_class_init (SbvConnectionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sbv_connection_finalize;
}

static void
sbv_connection_init (SbvConnection *self)
{
  g_mutex_init (&self->mutex);
}

SbvConnection *
sbv_connection_new (void)
{
  return g_object_new (SBV_TYPE_CONNECTION, NULL);
}

/* ── Accessors ─────────────────────────────────────────────────────────── */

const char *sbv_connection_get_host     (SbvConnection *self) { return self->host; }
const char *sbv_connection_get_base_dn  (SbvConnection *self) { return self->base_dn; }
int         sbv_connection_get_port     (SbvConnection *self) { return self->port; }
gboolean    sbv_connection_is_connected (SbvConnection *self) { return self->connected; }
/* Returns the SbvProfile this connection was bound with, or NULL if the
 * connection hasn't completed yet. The connection holds a ref; do not
 * free. */
SbvProfile *sbv_connection_get_profile  (SbvConnection *self) { return self->profile; }

/* ── LDAP handle acquire/release ───────────────────────────────────────── */

LDAP *
sbv_connection_acquire_ldap (SbvConnection *self)
{
  g_mutex_lock (&self->mutex);
  return self->ld;
}

void
sbv_connection_release_ldap (SbvConnection *self)
{
  g_mutex_unlock (&self->mutex);
}

/* ── Disconnect ────────────────────────────────────────────────────────── */

void
sbv_connection_disconnect (SbvConnection *self)
{
  g_mutex_lock (&self->mutex);
  if (self->ld) {
    ldap_unbind_ext_s (self->ld, NULL, NULL);
    self->ld = NULL;
  }
  self->connected = FALSE;
  g_mutex_unlock (&self->mutex);
}

/* ── GSSAPI interact callback ──────────────────────────────────────────── */

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

/* ── Connect thread ────────────────────────────────────────────────────── */

typedef struct {
  SbvProfile *profile;   /* owned */
  char       *password;  /* owned; NULL for Kerberos */
} ConnectData;

static void
connect_data_free (ConnectData *d)
{
  g_object_unref (d->profile);
  if (d->password) {
    memset (d->password, 0, strlen (d->password));
    g_free (d->password);
  }
  g_free (d);
}

static void
connect_thread (GTask *task, gpointer source, gpointer task_data,
                G_GNUC_UNUSED GCancellable *cancellable)
{
  SbvConnection *self    = SBV_CONNECTION (source);
  ConnectData   *d       = task_data;
  SbvProfile    *profile = d->profile;
  LDAP          *ld      = NULL;
  int            rc;
  int            ver     = LDAP_VERSION3;

  const char *host = sbv_profile_get_host (profile);
  int         port = sbv_profile_get_port (profile);

  char *uri = sbv_profile_get_use_ldaps (profile)
    ? g_strdup_printf ("ldaps://%s:%d", host, port)
    : g_strdup_printf ("ldap://%s:%d",  host, port);

  rc = ldap_initialize (&ld, uri);
  g_free (uri);

  if (rc != LDAP_SUCCESS) {
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "ldap_initialize: %s", ldap_err2string (rc));
    return;
  }

  ldap_set_option (ld, LDAP_OPT_PROTOCOL_VERSION, &ver);

  struct timeval timeout = { 10, 0 };
  ldap_set_option (ld, LDAP_OPT_NETWORK_TIMEOUT, &timeout);

  if (sbv_profile_get_skip_cert (profile)) {
    int never = LDAP_OPT_X_TLS_NEVER;
    ldap_set_option (NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &never);
    ldap_set_option (ld,   LDAP_OPT_X_TLS_REQUIRE_CERT, &never);
  }

  if (sbv_profile_get_use_tls (profile)) {
    rc = ldap_start_tls_s (ld, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      ldap_unbind_ext_s (ld, NULL, NULL);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_TLS,
                               "STARTTLS failed: %s", ldap_err2string (rc));
      return;
    }
  }

  if (sbv_profile_get_auth_type (profile) == SBV_AUTH_KERBEROS) {
    /* GSSAPI bind — uses the current Kerberos credential cache (KRB5CCNAME) */
    rc = ldap_sasl_interactive_bind_s (ld, NULL, "GSSAPI",
                                        NULL, NULL,
                                        LDAP_SASL_QUIET,
                                        gssapi_interact, NULL);
    if (rc != LDAP_SUCCESS) {
      ldap_unbind_ext_s (ld, NULL, NULL);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_AUTH,
                               "GSSAPI bind failed: %s\n"
                               "(Hint: run 'kinit user@REALM' first)",
                               ldap_err2string (rc));
      return;
    }
  } else {
    /* Simple bind */
    struct berval cred = {
      .bv_val = d->password,
      .bv_len = d->password ? strlen (d->password) : 0,
    };
    rc = ldap_sasl_bind_s (ld, sbv_profile_get_bind_dn (profile),
                            LDAP_SASL_SIMPLE, &cred, NULL, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
      ldap_unbind_ext_s (ld, NULL, NULL);
      g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_AUTH,
                               "Authentication failed: %s", ldap_err2string (rc));
      return;
    }
  }

  g_mutex_lock (&self->mutex);
  if (self->ld)
    ldap_unbind_ext_s (self->ld, NULL, NULL);
  self->ld        = ld;
  self->connected = TRUE;
  g_free (self->host);
  self->host    = g_strdup (host);
  self->port    = port;
  g_free (self->base_dn);
  self->base_dn = g_strdup (sbv_profile_get_base_dn (profile));
  g_clear_object (&self->profile);
  self->profile = g_object_ref (profile);
  g_mutex_unlock (&self->mutex);

  g_task_return_boolean (task, TRUE);
}

/* ── Public async connect ──────────────────────────────────────────────── */

void
sbv_connection_connect_profile_async (SbvConnection       *self,
                                       SbvProfile          *profile,
                                       const char          *password,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  ConnectData *d  = g_new0 (ConnectData, 1);
  d->profile      = g_object_ref (profile);
  d->password     = g_strdup (password);

  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, d, (GDestroyNotify) connect_data_free);
  g_task_run_in_thread (task, connect_thread);
  g_object_unref (task);
}

gboolean
sbv_connection_connect_finish (SbvConnection *self,
                                GAsyncResult  *result,
                                GError       **error)
{
  (void) self;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Who am I? (RFC 4532) ──────────────────────────────────────────────── */

/* Worker: invoke the LDAP "Who am I?" extended op and return the
 * server-confirmed identity string (with its `dn:` / `u:` scheme prefix
 * intact — the UI layer decides how to present it). */
static void
whoami_thread (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
  SbvConnection *self = SBV_CONNECTION (source);
  (void) task_data; (void) cancellable;

  LDAP *ld = sbv_connection_acquire_ldap (self);
  if (!ld) {
    sbv_connection_release_ldap (self);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_CONNECTION,
                             "Not connected");
    return;
  }

  struct berval *authzid = NULL;
  int rc = ldap_whoami_s (ld, &authzid, NULL, NULL);
  sbv_connection_release_ldap (self);

  if (rc != LDAP_SUCCESS) {
    if (authzid) ber_bvfree (authzid);
    g_task_return_new_error (task, SBV_ERROR, SBV_ERROR_LDAP,
                             "Who am I? failed: %s", ldap_err2string (rc));
    return;
  }

  char *out = (authzid && authzid->bv_val)
    ? g_strndup (authzid->bv_val, authzid->bv_len)
    : g_strdup ("");
  if (authzid) ber_bvfree (authzid);
  g_task_return_pointer (task, out, g_free);
}

void
sbv_connection_whoami_async (SbvConnection       *self,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_run_in_thread (task, whoami_thread);
  g_object_unref (task);
}

char *
sbv_connection_whoami_finish (SbvConnection *self,
                               GAsyncResult  *result,
                               GError       **error)
{
  (void) self;
  return g_task_propagate_pointer (G_TASK (result), error);
}
