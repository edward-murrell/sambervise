#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-domain.h"

G_BEGIN_DECLS

/* Domain-level settings: the password and lockout policy on the domain
 * root (R11), and the FSMO role holders (R14). */

/* Reads the policy attributes from the domain root. The finish call
 * returns a new SbvDomainPolicy, freed with g_free. */
void             sbv_domain_policy_read_async   (SbvConnection       *conn,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
SbvDomainPolicy *sbv_domain_policy_read_finish  (SbvConnection  *conn,
                                                 GAsyncResult   *result,
                                                 GError        **error);

/* Writes the fields of `updated` that differ from `original` in one
 * modify of the domain root. Both structs are copied. Nothing is sent
 * when they match. */
void             sbv_domain_policy_write_async  (SbvConnection         *conn,
                                                 const SbvDomainPolicy *original,
                                                 const SbvDomainPolicy *updated,
                                                 GCancellable          *cancellable,
                                                 GAsyncReadyCallback    callback,
                                                 gpointer               user_data);
gboolean         sbv_domain_policy_write_finish (SbvConnection  *conn,
                                                 GAsyncResult   *result,
                                                 GError        **error);

/* Reads the holder of each FSMO role, plus the two DNS partition roles
 * when those partitions exist. The finish call returns a GPtrArray of
 * SbvFsmoRole that frees its items. */
void             sbv_domain_fsmo_read_async     (SbvConnection       *conn,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
GPtrArray       *sbv_domain_fsmo_read_finish    (SbvConnection  *conn,
                                                 GAsyncResult   *result,
                                                 GError        **error);

G_END_DECLS
