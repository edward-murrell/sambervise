#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_DOMAIN_PANEL (sbv_domain_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvDomainPanel, sbv_domain_panel, SBV, DOMAIN_PANEL, GtkBox)

/* Panel for domain-wide settings: the password and lockout policy, which
 * can be edited, and the FSMO role holders, which are read-only.
 *
 * Emits "policy-changed" (no arguments) after a policy save, since
 * password expiry and lockout status in the Users panel depend on it. */
GtkWidget *sbv_domain_panel_new  (void);
void       sbv_domain_panel_load (SbvDomainPanel *self, SbvConnection *conn);

G_END_DECLS
