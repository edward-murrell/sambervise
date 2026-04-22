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

G_END_DECLS
