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

/* AD groupType bits — public so the create dialog can build the bitmask. */
#define SBV_GT_SECURITY     ((gint32) 0x80000000)
#define SBV_GT_BUILTIN      0x00000001
#define SBV_GT_GLOBAL       0x00000002
#define SBV_GT_DOMAIN_LOCAL 0x00000004
#define SBV_GT_UNIVERSAL    0x00000008

/* Create a new group in the supplied container. group_type is the raw
 * AD groupType bitmask (one scope bit OR optional SBV_GT_SECURITY). The DN
 * built is `CN=<sam>,<container_dn>`.
 *
 * Required: sam, container_dn, group_type. Optional: description.
 *
 * On success, returns the new DN via finish() (caller frees). */
void   sbv_groups_create_async  (SbvConnection       *conn,
                                  const char          *sam,
                                  const char          *description,
                                  gint32               group_type,
                                  const char          *container_dn,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);
char  *sbv_groups_create_finish (SbvConnection *conn,
                                  GAsyncResult  *result,
                                  GError       **error);

/* Permanently delete a group by DN. Irreversible. */
void     sbv_groups_delete_async  (SbvConnection       *conn,
                                    SbvGroup            *group,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
gboolean sbv_groups_delete_finish (SbvConnection *conn,
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
