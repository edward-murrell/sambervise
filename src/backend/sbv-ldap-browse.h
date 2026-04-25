#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-ldap-node.h"

G_BEGIN_DECLS

/* Fetch the immediate children of `parent_dn` (LDAP scope = ONELEVEL).
 * Returns a GListStore<SbvLdapNode> of the discovered entries; each node
 * carries its full attribute dump so the detail pane can render it
 * without a follow-up call. */
void        sbv_ldap_browse_children_async  (SbvConnection       *conn,
                                              const char          *parent_dn,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
GListStore *sbv_ldap_browse_children_finish (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

/* Fetch only the attributes of `dn` (scope = BASE). Useful for the root
 * node, which has no parent listing to inherit attributes from.
 * Returns a GHashTable<char*, GStrv> via finish (caller frees). */
void        sbv_ldap_browse_attrs_async     (SbvConnection       *conn,
                                              const char          *dn,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
GHashTable *sbv_ldap_browse_attrs_finish    (SbvConnection *conn,
                                              GAsyncResult  *result,
                                              GError       **error);

G_END_DECLS
