#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_CONTACTS_PANEL (sbv_contacts_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvContactsPanel, sbv_contacts_panel, SBV, CONTACTS_PANEL, GtkBox)

GtkWidget *sbv_contacts_panel_new  (void);
void       sbv_contacts_panel_load (SbvContactsPanel *self, SbvConnection *conn);

G_END_DECLS
