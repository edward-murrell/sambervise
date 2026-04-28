#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-computer.h"

G_BEGIN_DECLS

/* List all computer accounts. Result is GListStore<SbvComputer>. */
void        sbv_computers_list_async  (SbvConnection       *conn,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
GListStore *sbv_computers_list_finish (SbvConnection *conn,
                                        GAsyncResult  *result,
                                        GError       **error);

/* Enable or disable a computer account by modifying userAccountControl. */
void     sbv_computers_set_enabled_async  (SbvConnection       *conn,
                                            SbvComputer         *computer,
                                            gboolean             enabled,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean sbv_computers_set_enabled_finish (SbvConnection *conn,
                                            GAsyncResult  *result,
                                            GError       **error);

/* Create a new computer account in the supplied container.
 *
 * The created account has userAccountControl = WORKSTATION_TRUST_ACCOUNT |
 * ACCOUNTDISABLE, so it appears disabled until the workstation actually
 * joins (or an admin enables it). The DN built is `CN=<cn>,<container_dn>`,
 * sAMAccountName is `<cn>$` (AD convention).
 *
 * Required: cn, container_dn. Optional: dns_hostname, description.
 *
 * On success, returns the DN of the new object via finish() (caller frees). */
void   sbv_computers_create_async  (SbvConnection       *conn,
                                     const char          *cn,
                                     const char          *dns_hostname,
                                     const char          *description,
                                     const char          *container_dn,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data);
char  *sbv_computers_create_finish (SbvConnection *conn,
                                     GAsyncResult  *result,
                                     GError       **error);

/* Permanently delete a computer account by DN. Irreversible. */
void     sbv_computers_delete_async  (SbvConnection       *conn,
                                       SbvComputer         *computer,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data);
gboolean sbv_computers_delete_finish (SbvConnection *conn,
                                       GAsyncResult  *result,
                                       GError       **error);

/* Update description and dNSHostName.
 * Pass NULL for any field to skip it; pass "" to clear it. */
void     sbv_computers_update_attrs_async  (SbvConnection       *conn,
                                             SbvComputer         *computer,
                                             const char          *description,
                                             const char          *dns_hostname,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
gboolean sbv_computers_update_attrs_finish (SbvConnection *conn,
                                             GAsyncResult  *result,
                                             GError       **error);

/* Replace the full servicePrincipalName multi-valued attribute on a
 * computer account.
 *
 * `spns` is a NULL-terminated array of strings (a GStrv); pass an empty
 * or NULL array to remove all SPNs (LDAP_MOD_REPLACE with no values
 * deletes the attribute). Caller retains ownership of `spns`; the call
 * copies what it needs.
 *
 * Note: SPNs are only functional when the backing computer account has
 * key material (i.e. has been joined). Callers should refuse to invoke
 * this on accounts where pwdLastSet == 0 — the request will succeed at
 * the directory level but the SPN will never be usable for Kerberos. */
void     sbv_computers_set_spn_async  (SbvConnection       *conn,
                                        SbvComputer         *computer,
                                        const char * const  *spns,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
gboolean sbv_computers_set_spn_finish (SbvConnection *conn,
                                        GAsyncResult  *result,
                                        GError       **error);

G_END_DECLS
