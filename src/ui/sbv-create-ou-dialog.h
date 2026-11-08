#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

/* Invoked once the OU has been created. `new_dn` is borrowed. */
typedef void (*SbvCreateOuCallback) (SbvConnection *conn,
                                     const char    *new_dn,
                                     gpointer       user_data);

/* Show a modal "Create OU" dialog. The parent DN defaults to `parent_dn`,
 * or the connection's base DN when that is NULL, and can be edited. On
 * success the dialog closes and `callback` runs. */
void sbv_create_ou_dialog_show (GtkWindow           *parent,
                                SbvConnection       *conn,
                                const char          *parent_dn,
                                SbvCreateOuCallback  callback,
                                gpointer             user_data);

G_END_DECLS
