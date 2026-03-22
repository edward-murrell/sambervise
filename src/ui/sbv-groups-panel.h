#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_GROUPS_PANEL (sbv_groups_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvGroupsPanel, sbv_groups_panel, SBV, GROUPS_PANEL, GtkBox)

GtkWidget *sbv_groups_panel_new  (void);
void       sbv_groups_panel_load (SbvGroupsPanel *self, SbvConnection *conn);

G_END_DECLS
