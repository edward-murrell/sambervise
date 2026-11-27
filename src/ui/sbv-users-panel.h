#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../backend/sbv-idmap-hints.h"

G_BEGIN_DECLS

#define SBV_TYPE_USERS_PANEL (sbv_users_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvUsersPanel, sbv_users_panel, SBV, USERS_PANEL, GtkBox)

GtkWidget *sbv_users_panel_new  (void);
void       sbv_users_panel_load (SbvUsersPanel *self, SbvConnection *conn);

/* Gives the panel the connection's ID range hints, which it copies.
 * Suggest uses them, and a rid domain gets a note on each user. */
void       sbv_users_panel_set_idmap_hints (SbvUsersPanel       *self,
                                            const SbvIdmapHints *hints);

G_END_DECLS
