#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

/* Invoked once a contact has been successfully created on the DC. */
typedef void (*SbvCreateContactCallback) (SbvConnection *conn,
                                           const char    *new_dn,
                                           gpointer       user_data);

/* Show a modal "Create Contact" dialog parented at `parent`. The container
 * defaults to `CN=Users,<base-dn>` (the AD home for contacts) but can be
 * overridden. On success the dialog closes itself and `callback` is invoked. */
void sbv_create_contact_dialog_show (GtkWindow               *parent,
                                      SbvConnection           *conn,
                                      SbvCreateContactCallback callback,
                                      gpointer                 user_data);

G_END_DECLS
