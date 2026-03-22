#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Query _ldap._tcp.dc._msdcs.<domain> (and fall back to _ldap._tcp.<domain>)
 * for LDAP SRV records advertising Samba / AD domain controllers.
 *
 * The result passed to GAsyncReadyCallback is a GListStore<GSrvTarget>
 * (via g_task_propagate_pointer).  Call sbv_dns_discover_finish() to
 * obtain it; the caller owns the returned store.
 */
void        sbv_dns_discover_async  (const char          *domain,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);

/* Returns GListStore<GSrvTarget> or NULL on error. Caller owns the store. */
GListStore *sbv_dns_discover_finish (GAsyncResult *result, GError **error);

G_END_DECLS
