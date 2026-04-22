#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_COMPUTERS_PANEL (sbv_computers_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvComputersPanel, sbv_computers_panel, SBV, COMPUTERS_PANEL, GtkBox)

GtkWidget *sbv_computers_panel_new  (void);
void       sbv_computers_panel_load (SbvComputersPanel *self, SbvConnection *conn);

G_END_DECLS
