#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "../model/sbv-profile.h"

G_BEGIN_DECLS

#define SBV_TYPE_CONNECTION (sbv_connection_get_type ())
G_DECLARE_FINAL_TYPE (SbvConnection, sbv_connection, SBV, CONNECTION, GObject)

SbvConnection *sbv_connection_new (void);

/* Accessors — read-only after a successful connect */
const char *sbv_connection_get_host    (SbvConnection *self);
const char *sbv_connection_get_base_dn (SbvConnection *self);
int         sbv_connection_get_port    (SbvConnection *self);
gboolean    sbv_connection_is_connected (SbvConnection *self);
SbvProfile *sbv_connection_get_profile (SbvConnection *self);

/* Connect using a saved profile.
 * password is required when profile uses SBV_AUTH_SIMPLE; pass NULL for
 * SBV_AUTH_KERBEROS (uses the current Kerberos credential cache). */
void     sbv_connection_connect_profile_async  (SbvConnection       *self,
                                                 SbvProfile          *profile,
                                                 const char          *password,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
gboolean sbv_connection_connect_finish         (SbvConnection *self,
                                                 GAsyncResult  *result,
                                                 GError       **error);

void     sbv_connection_disconnect (SbvConnection *self);

/* Internal: acquire/release the LDAP handle for use in backend threads.
 * Always call release after acquire, even on error. */
struct ldap *sbv_connection_acquire_ldap (SbvConnection *self);
void         sbv_connection_release_ldap (SbvConnection *self);

G_END_DECLS
