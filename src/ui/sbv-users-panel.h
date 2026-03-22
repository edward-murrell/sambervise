#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_USERS_PANEL (sbv_users_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvUsersPanel, sbv_users_panel, SBV, USERS_PANEL, GtkBox)

GtkWidget *sbv_users_panel_new  (void);
void       sbv_users_panel_load (SbvUsersPanel *self, SbvConnection *conn);

G_END_DECLS
