#pragma once

#include <gio/gio.h>
#include "../model/sbv-profile.h"
#include "sbv-idmap-hints.h"

G_BEGIN_DECLS

/* Result of a connect-dialog DC probe. `rootdse` is always populated on
 * success (anonymous BASE search of the empty DN). When the caller
 * supplies a bind profile, the probe also binds and reads the well-known
 * idmap location, populating `idmap`; otherwise `idmap` is NULL. If the
 * bind itself failed, the anonymous read still succeeds and
 * `bind_error` carries a human-readable reason. */
typedef struct {
  GHashTable    *rootdse;     /* GHashTable<char*, GStrv> — owned        */
  SbvIdmapHints *idmap;       /* owned; NULL if no bind                  */
  char          *bind_error;  /* owned; NULL on success / no bind        */
} SbvProbeResult;

/* Free a probe result and all its members. NULL-safe. */
void sbv_probe_result_free (SbvProbeResult *r);

/* One-shot LDAP probe of a candidate DC. Used by the connect dialog's
 * "Probe DC" button. Always reads the RootDSE anonymously to verify
 * reachability and surface naming-context / SASL-mech facts.
 *
 * If `bind_profile` is non-NULL the probe additionally binds (using the
 * profile's auth type — Kerberos or simple, matching `sbv-connection.c`)
 * and reads the well-known SFU30 idmap location to populate
 * `SbvProbeResult.idmap`. The profile's UID/GID range fields are merged
 * into the returned hints in the same way `sbv_idmap_hints_query_async`
 * does for live connections. The bind step is best-effort: a failure
 * surfaces in `bind_error` without aborting the anonymous probe. */
void            sbv_probe_dc_async   (const char          *host,
                                       int                  port,
                                       gboolean             use_ldaps,
                                       gboolean             use_starttls,
                                       gboolean             skip_cert,
                                       SbvProfile          *bind_profile,
                                       const char          *bind_password,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);
SbvProbeResult *sbv_probe_dc_finish  (GAsyncResult        *result,
                                       GError             **error);

G_END_DECLS
