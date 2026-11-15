#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-domain.h"

G_BEGIN_DECLS

/* Fine-grained password policies (R12): msDS-PasswordSettings objects in
 * CN=Password Settings Container,CN=System,<base>. */

/* Lists every policy, sorted by precedence. A missing container gives an
 * empty list. The finish call returns a GPtrArray of SbvPso that frees
 * its items. */
void       sbv_pso_list_async    (SbvConnection       *conn,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
GPtrArray *sbv_pso_list_finish   (SbvConnection  *conn,
                                  GAsyncResult   *result,
                                  GError        **error);

/* Creates `pso` in the container. `pso` is copied and its `dn` is
 * ignored. The finish call returns the new DN. Caller frees. */
void       sbv_pso_create_async  (SbvConnection       *conn,
                                  const SbvPso        *pso,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
char      *sbv_pso_create_finish (SbvConnection  *conn,
                                  GAsyncResult   *result,
                                  GError        **error);

/* Writes what differs between `original` and `updated` to the policy at
 * `original->dn`, then renames it when the name changed. Both are
 * copied. The finish call returns the policy's DN after any rename.
 * Caller frees. */
void       sbv_pso_update_async  (SbvConnection       *conn,
                                  const SbvPso        *original,
                                  const SbvPso        *updated,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
char      *sbv_pso_update_finish (SbvConnection  *conn,
                                  GAsyncResult   *result,
                                  GError        **error);

/* Deletes the policy at `dn`. Accounts it applied to fall back to the
 * next policy or the domain policy. */
void       sbv_pso_delete_async  (SbvConnection       *conn,
                                  const char          *dn,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
gboolean   sbv_pso_delete_finish (SbvConnection  *conn,
                                  GAsyncResult   *result,
                                  GError        **error);

G_END_DECLS
