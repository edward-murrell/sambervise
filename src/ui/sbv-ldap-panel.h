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
/* Re-reads the open entry from the DC and drops unsaved edits. Keeps the
 * tree as it is. Does nothing before the first load. */
void       sbv_ldap_panel_refresh (SbvLdapPanel *self);

G_END_DECLS
