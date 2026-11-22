#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_SITES_PANEL (sbv_sites_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvSitesPanel, sbv_sites_panel, SBV, SITES_PANEL, GtkBox)

/* Panel for sites and subnets (R13). Lists each site with its DCs, and
 * each subnet with its site. Both can be created, edited and deleted. */
GtkWidget *sbv_sites_panel_new  (void);
void       sbv_sites_panel_load (SbvSitesPanel *self, SbvConnection *conn);

G_END_DECLS
