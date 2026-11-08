#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_OUS_PANEL (sbv_ous_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvOusPanel, sbv_ous_panel, SBV, OUS_PANEL, GtkBox)

/* Panel for browsing and managing organisational units.
 *
 * Emits "directory-changed" (no arguments) after an OU is renamed, moved
 * or deleted. Those operations change the DN of everything beneath the
 * OU, so other panels holding DNs should reload. */
GtkWidget *sbv_ous_panel_new  (void);
void       sbv_ous_panel_load (SbvOusPanel *self, SbvConnection *conn);

G_END_DECLS
