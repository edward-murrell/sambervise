#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_DNS_PANEL (sbv_dns_panel_get_type ())
G_DECLARE_FINAL_TYPE (SbvDnsPanel, sbv_dns_panel, SBV, DNS_PANEL, GtkBox)

/* Panel for AD-integrated DNS: a list of zones, and the records of the
 * selected zone. A, AAAA, CNAME and PTR records can be added, edited and
 * deleted. Other types are shown read-only. */
GtkWidget *sbv_dns_panel_new  (void);
void       sbv_dns_panel_load (SbvDnsPanel *self, SbvConnection *conn);

G_END_DECLS
