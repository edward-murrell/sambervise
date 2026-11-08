#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-ou.h"

G_BEGIN_DECLS

/* List the domain root plus every OU and container beneath it that the DC
 * doesn't hide as "advanced view only". This matches the containers ADUC
 * shows by default and excludes CN=System and its children. Result is a
 * GListStore<SbvOu> in tree order, domain root first. */
void        sbv_ou_list_async   (SbvConnection       *conn,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data);
GListStore *sbv_ou_list_finish  (SbvConnection *conn,
                                 GAsyncResult  *result,
                                 GError       **error);

/* Create `OU=<name>,<parent_dn>`. `description` may be NULL or empty.
 * Returns the new DN on success (caller frees), NULL on error. */
void   sbv_ou_create_async  (SbvConnection       *conn,
                             const char          *parent_dn,
                             const char          *name,
                             const char          *description,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);
char  *sbv_ou_create_finish (SbvConnection *conn,
                             GAsyncResult  *result,
                             GError       **error);

/* Rename an OU and/or change its description. `new_name` NULL or equal to
 * the current name skips the rename. `description` is tri-state: NULL
 * leaves it alone, "" clears it, anything else replaces it. Renaming an OU
 * changes the DN of every object beneath it. Returns the OU's DN after the
 * change (caller frees), NULL on error. */
void   sbv_ou_update_async  (SbvConnection       *conn,
                             SbvOu               *ou,
                             const char          *new_name,
                             const char          *description,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);
char  *sbv_ou_update_finish (SbvConnection *conn,
                             GAsyncResult  *result,
                             GError       **error);

/* Delete an empty OU. Fails with a readable message if the OU still has
 * children or the DC refuses (e.g. accidental-deletion protection). */
void     sbv_ou_delete_async  (SbvConnection       *conn,
                               SbvOu               *ou,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);
gboolean sbv_ou_delete_finish (SbvConnection *conn,
                               GAsyncResult  *result,
                               GError       **error);

/* Move any object (user, group, computer, contact or OU) under
 * `new_parent_dn`, keeping its RDN. Refuses to move an object into its
 * current container or into itself. Returns the new DN on success (caller
 * frees), NULL on error. */
void   sbv_ou_move_object_async  (SbvConnection       *conn,
                                  const char          *dn,
                                  const char          *new_parent_dn,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
char  *sbv_ou_move_object_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error);

G_END_DECLS
