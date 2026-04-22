#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-group.h"

G_BEGIN_DECLS

/* List all groups. Result is GListStore<SbvGroup>. */
void       sbv_groups_list_async  (SbvConnection       *conn,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
GListStore *sbv_groups_list_finish (SbvConnection *conn,
                                    GAsyncResult  *result,
                                    GError       **error);

/* Add a user DN to a group. */
void     sbv_groups_add_member_async  (SbvConnection       *conn,
                                        SbvGroup            *group,
                                        const char          *member_dn,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
gboolean sbv_groups_add_member_finish (SbvConnection *conn,
                                        GAsyncResult  *result,
                                        GError       **error);

/* Remove a user DN from a group. */
void     sbv_groups_remove_member_async  (SbvConnection       *conn,
                                           SbvGroup            *group,
                                           const char          *member_dn,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
gboolean sbv_groups_remove_member_finish (SbvConnection *conn,
                                           GAsyncResult  *result,
                                           GError       **error);

/* Set RFC2307/POSIX gidNumber. Pass -1 to clear. */
void     sbv_groups_set_unix_attrs_async  (SbvConnection       *conn,
                                            SbvGroup            *group,
                                            gint                 gid_number,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean sbv_groups_set_unix_attrs_finish (SbvConnection *conn,
                                            GAsyncResult  *result,
                                            GError       **error);

G_END_DECLS
