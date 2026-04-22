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

/* Update name and contact attributes (cn, displayName, givenName, sn, mail).
 * Pass NULL for any field to skip it; pass "" to clear it. */
void     sbv_users_update_attrs_async  (SbvConnection       *conn,
                                         SbvUser             *user,
                                         const char          *cn,
                                         const char          *display_name,
                                         const char          *given_name,
                                         const char          *sn,
                                         const char          *email,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
gboolean sbv_users_update_attrs_finish (SbvConnection *conn,
                                         GAsyncResult  *result,
                                         GError       **error);

/* Set password policy flags in one LDAP call.
 * force_change   : TRUE sets pwdLastSet=0 (must change at next logon);
 *                  FALSE sets pwdLastSet=-1 (resets to current time).
 * never_expires  : toggles ADS_UF_DONT_EXPIRE_PASSWD in userAccountControl.
 * account_expires: G_MAXINT64 = never; otherwise a Windows FILETIME value. */
void     sbv_users_set_password_flags_async  (SbvConnection       *conn,
                                               SbvUser             *user,
                                               gboolean             force_change,
                                               gboolean             never_expires,
                                               gint64               account_expires,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
gboolean sbv_users_set_password_flags_finish (SbvConnection *conn,
                                               GAsyncResult  *result,
                                               GError       **error);

/* Set RFC2307/POSIX Unix attributes. Pass -1 for uid/gid to clear; "" to clear strings. */
void     sbv_users_set_unix_attrs_async  (SbvConnection       *conn,
                                           SbvUser             *user,
                                           gint                 uid_number,
                                           gint                 gid_number,
                                           const char          *login_shell,
                                           const char          *home_dir,
                                           const char          *gecos,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data);
gboolean sbv_users_set_unix_attrs_finish (SbvConnection *conn,
                                           GAsyncResult  *result,
                                           GError       **error);

G_END_DECLS
