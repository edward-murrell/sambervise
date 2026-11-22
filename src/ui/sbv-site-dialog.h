#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"
#include "../model/sbv-site.h"

G_BEGIN_DECLS

/* Runs after a site or subnet is created, saved or deleted. `message`
 * suits a toast, e.g. "Site created". */
typedef void (*SbvSiteDoneFn) (const char *message, gpointer user_data);

/* Opens the dialog that creates a site, when `site` is NULL, or edits
 * one. `sites_dn` is CN=Sites in the Configuration partition.
 * `n_subnets` is how many subnets map to the site, for the delete
 * warning. `done` runs after a change succeeds, and `user_data_free`,
 * if set, frees `user_data` when the dialog goes. */
void sbv_site_dialog_show   (GtkWindow      *parent,
                             SbvConnection  *conn,
                             const char     *sites_dn,
                             const SbvSite  *site,
                             guint           n_subnets,
                             SbvSiteDoneFn   done,
                             gpointer        user_data,
                             GDestroyNotify  user_data_free);

/* Opens the dialog that creates a subnet, when `subnet` is NULL, or
 * edits one. `sites` is a GPtrArray of SbvSite to choose from. It is
 * read during the call only. Callbacks work as for the site dialog. */
void sbv_subnet_dialog_show (GtkWindow        *parent,
                             SbvConnection    *conn,
                             const char       *sites_dn,
                             const SbvSubnet  *subnet,
                             GPtrArray        *sites,
                             SbvSiteDoneFn     done,
                             gpointer          user_data,
                             GDestroyNotify    user_data_free);

G_END_DECLS
