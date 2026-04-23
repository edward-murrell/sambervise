#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

/* Invoked once a computer account has been successfully created on the DC.
 * The new DN is provided so the caller can refresh and select the entry. */
typedef void (*SbvCreateComputerCallback) (SbvConnection *conn,
                                            const char    *new_dn,
                                            gpointer       user_data);

/* Show a modal "Create Computer" dialog parented at `parent`. The container
 * defaults to `CN=Computers,<base-dn>`; sAMAccountName is auto-derived as
 * `<cn>$` per AD convention. The account is created disabled with
 * userAccountControl=WORKSTATION_TRUST_ACCOUNT|ACCOUNTDISABLE. */
void sbv_create_computer_dialog_show (GtkWindow                 *parent,
                                       SbvConnection             *conn,
                                       SbvCreateComputerCallback  callback,
                                       gpointer                   user_data);

G_END_DECLS
