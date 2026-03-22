#pragma once

#include <gio/gio.h>
#include "sbv-connection.h"
#include "../model/sbv-user.h"

G_BEGIN_DECLS

/* List all users. Result is GListStore<SbvUser>. */
void       sbv_users_list_async  (SbvConnection       *conn,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
GListStore *sbv_users_list_finish (SbvConnection *conn,
                                   GAsyncResult  *result,
                                   GError       **error);

/* Enable or disable a user account by modifying userAccountControl. */
void     sbv_users_set_enabled_async  (SbvConnection       *conn,
                                        SbvUser             *user,
                                        gboolean             enabled,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
gboolean sbv_users_set_enabled_finish (SbvConnection *conn,
                                        GAsyncResult  *result,
                                        GError       **error);

/* Reset a user's password. Requires LDAPS or STARTTLS. */
void     sbv_users_reset_password_async  (SbvConnection       *conn,
                                           SbvUser             *user,
                                           const char          *new_password,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
gboolean sbv_users_reset_password_finish (SbvConnection *conn,
                                           GAsyncResult  *result,
                                           GError       **error);

G_END_DECLS
