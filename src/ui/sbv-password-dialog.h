#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../model/sbv-profile.h"

G_BEGIN_DECLS

typedef void (*SbvPasswordCallback) (SbvConnection *connection,
                                      gpointer       user_data);

/* Shows a modal password-entry dialog for a saved simple-auth profile.
 * On success, calls callback with a ready SbvConnection.
 * On cancel or error, callback is not called. */
void sbv_password_dialog_show (GtkWindow          *parent,
                                SbvProfile         *profile,
                                SbvPasswordCallback callback,
                                gpointer            user_data);

G_END_DECLS
