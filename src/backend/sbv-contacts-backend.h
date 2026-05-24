#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-contact.h"

G_BEGIN_DECLS

/* List all `contact` objects (objectClass=contact, excluding users) under the
 * connection's base DN. Result is GListStore<SbvContact>. */
void        sbv_contacts_list_async  (SbvConnection       *conn,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);
GListStore *sbv_contacts_list_finish (SbvConnection *conn,
                                       GAsyncResult  *result,
                                       GError       **error);

/* Create a new contact (objectClass=contact) at `CN=<cn>,<container_dn>`.
 * Required: cn, container_dn. Optional fields may be NULL or empty.
 * Returns the new DN on success (caller frees), NULL on error. */
void   sbv_contacts_create_async  (SbvConnection       *conn,
                                    const char          *cn,
                                    const char          *given_name,
                                    const char          *sn,
                                    const char          *mail,
                                    const char          *telephone_number,
                                    const char          *container_dn,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
char  *sbv_contacts_create_finish (SbvConnection *conn,
                                    GAsyncResult  *result,
                                    GError       **error);

/* Permanently delete a contact by DN. Irreversible. */
void     sbv_contacts_delete_async  (SbvConnection       *conn,
                                      SbvContact          *contact,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gboolean sbv_contacts_delete_finish (SbvConnection *conn,
                                      GAsyncResult  *result,
                                      GError       **error);

/* Update mutable attributes on an existing contact.
 *
 * Each scalar parameter is tri-state: NULL means "leave alone", "" means
 * "clear the attribute", anything else replaces the value. `proxy_addresses`
 * is a NULL-or-empty-terminated vector; passing NULL leaves it alone, an
 * empty vector clears the attribute, otherwise the full vector replaces it.
 *
 * If `cn` is non-NULL and non-empty, the entry is renamed (CN is the RDN)
 * before the other modifications run. */
void     sbv_contacts_update_async  (SbvConnection       *conn,
                                      SbvContact          *contact,
                                      const char          *cn,
                                      const char          *display_name,
                                      const char          *given_name,
                                      const char          *sn,
                                      const char          *description,
                                      const char          *mail,
                                      const char *const   *proxy_addresses,
                                      const char          *telephone_number,
                                      const char          *mobile,
                                      const char          *fax,
                                      const char          *office,
                                      const char          *street_address,
                                      const char          *locality,
                                      const char          *state,
                                      const char          *postal_code,
                                      const char          *country,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gboolean sbv_contacts_update_finish (SbvConnection *conn,
                                      GAsyncResult  *result,
                                      GError       **error);

G_END_DECLS
