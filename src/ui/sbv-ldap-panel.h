#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_LDAP_PANEL (sbv_ldap_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvLdapPanel, sbv_ldap_panel, SBV, LDAP_PANEL, GtkBox)

GtkWidget *sbv_ldap_panel_new  (void);

/* Reload the panel against `conn`. Resets the tree to show only the
 * connection's base DN as the root node; children are loaded lazily on
 * expand. */
void       sbv_ldap_panel_load (SbvLdapPanel *self, SbvConnection *conn);

G_END_DECLS
