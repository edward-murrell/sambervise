#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

/* Invoked once a group has been successfully created on the DC. The created
 * DN is provided so the caller can refresh and (optionally) select it. */
typedef void (*SbvCreateGroupCallback) (SbvConnection *conn,
                                         const char    *new_dn,
                                         gpointer       user_data);

/* Show a modal "Create Group" dialog parented at `parent`. The container
 * defaults to `CN=Users,<base-dn>` (matches AD's default for new groups);
 * the scope defaults to Global and the type defaults to Security. On
 * success the dialog closes itself and `callback` is invoked. */
void sbv_create_group_dialog_show (GtkWindow             *parent,
                                    SbvConnection         *conn,
                                    SbvCreateGroupCallback callback,
                                    gpointer               user_data);

G_END_DECLS
