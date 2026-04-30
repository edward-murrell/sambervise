#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* One-shot anonymous LDAP probe of a candidate DC. Used by the connect
 * dialog's "Probe DC" button to verify reachability and surface RootDSE
 * facts (defaultNamingContext, dnsHostName, supported SASL mechanisms,
 * domain functionality level) before the user commits to a profile. The
 * probe does not bind, so it works on DCs that allow anonymous reads of
 * the DSE (the AD default).
 *
 * Result on success is a GHashTable<char*, GStrv> keyed by attribute
 * name, matching the shape used by the rest of the codebase. */
void        sbv_probe_dc_async   (const char          *host,
                                   int                  port,
                                   gboolean             use_ldaps,
                                   gboolean             use_starttls,
                                   gboolean             skip_cert,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
GHashTable *sbv_probe_dc_finish  (GAsyncResult  *result,
                                   GError       **error);

G_END_DECLS
