#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../model/sbv-user.h"

G_BEGIN_DECLS

/* Invoked once a user has been successfully created on the DC. The created
 * DN is provided so the caller can refresh and select the new entry. */
typedef void (*SbvCreateUserCallback) (SbvConnection *conn,
                                        const char    *new_dn,
                                        gpointer       user_data);

/* Show a modal "Create User" dialog parented at `parent`. The container
 * defaults to `CN=Users,<base-dn>` but can be overridden in the dialog.
 * On success the dialog closes itself and `callback` is invoked. On cancel
 * or error the callback is not called. */
void sbv_create_user_dialog_show (GtkWindow            *parent,
                                   SbvConnection        *conn,
                                   SbvCreateUserCallback callback,
                                   gpointer              user_data);

/* Show the same dialog in copy mode. The container starts as the
 * template's. After the user is created, the template's groups, Windows
 * profile paths, account options and expiry are copied onto it (see
 * sbv_bulk_apply_user_template_async). `callback` runs after that step,
 * even if part of it failed. Failures are listed in the dialog, which
 * then offers Close. */
void sbv_create_user_dialog_show_copy (GtkWindow            *parent,
                                        SbvConnection        *conn,
                                        SbvUser              *template_user,
                                        SbvCreateUserCallback callback,
                                        gpointer              user_data);

G_END_DECLS
